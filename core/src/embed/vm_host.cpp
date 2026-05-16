#include "fleaux/embed/vm_host.hpp"

#include "fleaux/embed/binding_plugin.hpp"
#include "fleaux/embed/dynamic_loader.hpp"
#include "fleaux/embed/native_bindings.hpp"
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/value.hpp"
namespace fleaux::embed {
namespace {
[[nodiscard]] auto make_error(const HostErrorCategory category, const std::string& message,
                              const std::optional<std::string>& hint = std::nullopt,
                              const std::optional<frontend::diag::SourceSpan>& span = std::nullopt) -> HostError {
  return HostError{
      .category = category,
      .message = message,
      .hint = hint,
      .span = span,
  };
}

[[nodiscard]] auto host_register_callable(NativeBindingRegistry* registry, NativeBinding binding) -> BindingResult {
  if (registry == nullptr) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInternal,
        .message = "Binding registry is null.",
        .symbol = std::move(binding.symbol),
    });
  }
  return registry->register_callable(std::move(binding));
}
}  // namespace
struct VmHost::Impl {
  struct LoadedBindingModule {
    std::unique_ptr<DynamicLibrary> library{};
    NativeBindingRegistry* registry{nullptr};
    std::vector<std::string> registered_symbols{};
  };

  explicit Impl(VmHostConfig initial_config)
      : config(std::move(initial_config)), owned_dynamic_loader(make_system_dynamic_loader()) {}

  Impl(const Impl& other)
      : config(other.config),
        owned_dynamic_loader(make_system_dynamic_loader()),
        dynamic_loader(owned_dynamic_loader.get()),
        runtime(other.runtime) {}

  auto operator=(const Impl& other) -> Impl& {
    if (this == &other) {
      return *this;
    }
    config = other.config;
    runtime = other.runtime;
    loaded_binding_modules.clear();
    owned_dynamic_loader = make_system_dynamic_loader();
    dynamic_loader = owned_dynamic_loader.get();
    return *this;
  }

  VmHostConfig config;
  std::unique_ptr<DynamicLoader> owned_dynamic_loader{};
  DynamicLoader* dynamic_loader{owned_dynamic_loader.get()};
  std::vector<LoadedBindingModule> loaded_binding_modules{};
  std::optional<bytecode::Module> last_fleaux_module{std::nullopt};
  vm::Runtime runtime{};

  ~Impl() { unload_binding_modules(); }
  void emit_stdout(const std::string_view text) const {
    if (text.empty()) {
      return;
    }
    if (config.stdout_sink.has_value()) {
      (*config.stdout_sink)(text);
      return;
    }
    std::cout << text;
  }
  void set_process_args_for_run(const std::string& entry_label) const {
    std::vector<std::string> args_storage;
    args_storage.reserve(config.process_args.size() + 1U);
    args_storage.push_back(entry_label);
    args_storage.insert(args_storage.end(), config.process_args.begin(), config.process_args.end());
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) {
      argv_ptrs.push_back(arg.data());
    }
    runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  }
  [[nodiscard]] auto resolve_entry_path(const std::filesystem::path& path) const -> std::optional<std::filesystem::path> {
    if (path.is_absolute() && std::filesystem::exists(path)) {
      return std::filesystem::weakly_canonical(path);
    }
    if (std::filesystem::exists(path)) {
      return std::filesystem::weakly_canonical(path);
    }
    for (const auto& root : config.import_roots) {
      const auto candidate = root / path;
      if (std::filesystem::exists(candidate)) {
        return std::filesystem::weakly_canonical(candidate);
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] auto execute_module(const bytecode::Module& module, const std::string& entry_label) const -> VmResult {
    set_process_args_for_run(entry_label);
    std::ostringstream captured_stdout;
    const auto exec_result = runtime.execute(module, captured_stdout);
    emit_stdout(captured_stdout.str());
    if (!exec_result) {
      return tl::unexpected(make_error(HostErrorCategory::kRuntime, exec_result.error().message, exec_result.error().hint,
                                       exec_result.error().span));
    }
    return runtime::make_int(static_cast<runtime::Int>(exec_result->exit_code));
  }

  void unload_binding_modules() {
    for (auto module_it = loaded_binding_modules.rbegin(); module_it != loaded_binding_modules.rend(); ++module_it) {
      if (module_it->registry != nullptr) {
        for (const auto& symbol : module_it->registered_symbols) {
          (void)module_it->registry->unregister_callable(symbol);
        }
      }
    }
    loaded_binding_modules.clear();
  }
};
VmHost::VmHost(const VmHostConfig& config) : impl_(std::move(Impl{config})) {}
VmHost::VmHost(const VmHost& other) = default;
VmHost::VmHost(VmHost&& other) noexcept = default;
auto VmHost::operator=(const VmHost& other) -> VmHost& = default;
auto VmHost::operator=(VmHost&& other) noexcept -> VmHost& = default;
VmHost::~VmHost() = default;
auto VmHost::run_file(const std::filesystem::path& path) const -> VmResult {
  const auto resolved_path = impl_->resolve_entry_path(path);
  if (!resolved_path.has_value()) {
    return tl::unexpected(make_error(HostErrorCategory::kIo,
                                     "Failed to locate source file: '" + path.string() + "'.",
                                     std::optional<std::string>{"Check the path or add an import root."}));
  }
  auto module_result = bytecode::load_linked_module(*resolved_path,
                                                    bytecode::ModuleLoadOptions{
                                                        .mode = bytecode::OptimizationMode::kBaseline,
                                                        .write_bytecode_cache = true,
                                                        .enable_value_ref_gate = impl_->config.compile_options.enable_value_ref_gate,
                                                        .enable_auto_value_ref = impl_->config.compile_options.enable_auto_value_ref,
                                                        .value_ref_byte_cutoff =
                                                            impl_->config.compile_options.value_ref_byte_cutoff,
                                                    });
  if (!module_result) {
    return tl::unexpected(make_error(HostErrorCategory::kBytecode, module_result.error().message));
  }

  impl_->last_fleaux_module = std::move(*module_result);
  return impl_->execute_module(*impl_->last_fleaux_module, resolved_path->string());
}
auto VmHost::run_source(const std::string_view module_name, const std::string_view source_text) const -> VmResult {
  if (module_name.empty()) {
    return tl::unexpected(make_error(HostErrorCategory::kIo,
                                     "Module name is required for run_source.",
                                     std::optional<std::string>{"Pass a synthetic file-like module name for diagnostics."}));
  }
  constexpr frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(std::string{source_text}, std::string{module_name});
  if (!parsed) {
    return tl::unexpected(
        make_error(HostErrorCategory::kParse, parsed.error().message, parsed.error().hint, parsed.error().span));
  }
  constexpr frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());
  if (!lowered) {
    return tl::unexpected(
        make_error(HostErrorCategory::kAnalysis, lowered.error().message, lowered.error().hint, lowered.error().span));
  }
  const auto seed_root = impl_->config.import_roots.empty() ? std::filesystem::path{} : impl_->config.import_roots.front();
  const auto source_path = seed_root / std::filesystem::path(module_name);
  const auto analyzed = frontend::source_loader::analyze_lowered_program_with_imports<HostError>(
      lowered.value(), source_path,
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<frontend::diag::SourceSpan>& span) -> HostError {
        return make_error(HostErrorCategory::kAnalysis, message, hint, span);
      });
  if (!analyzed) {
    return tl::unexpected(analyzed.error());
  }
  const bytecode::CompileOptions compile_options{
      .source_path = source_path,
      .source_text = std::string{source_text},
      .module_name = std::string{module_name},
      .imported_modules = {},
      .enable_value_ref_gate = impl_->config.compile_options.enable_value_ref_gate,
      .enable_auto_value_ref = impl_->config.compile_options.enable_auto_value_ref,
      .value_ref_byte_cutoff = impl_->config.compile_options.value_ref_byte_cutoff,
  };
  const bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(*analyzed, compile_options);
  if (!compiled) {
    return tl::unexpected(make_error(HostErrorCategory::kBytecode, compiled.error().message));
  }

  impl_->last_fleaux_module = std::move(*compiled);
  return impl_->execute_module(*impl_->last_fleaux_module, std::string{module_name});
}
auto VmHost::call_native(const std::string_view qualified_symbol, const VmValue& args) const -> VmResult {
  auto* registry = impl_->config.binding_registry;
  if (registry == nullptr) {
    return tl::unexpected(
        make_error(HostErrorCategory::kBinding,
                   "No binding registry configured for VmHost.call_native.",
                   std::optional<std::string>{"Set VmHostConfig.binding_registry or call set_binding_registry()."}));
  }

  const auto* binding = registry->find_callable(qualified_symbol);
  if (binding == nullptr) {
    return tl::unexpected(make_error(
        HostErrorCategory::kBinding,
        "Binding symbol not found: '" + std::string{qualified_symbol} + "'.",
        std::optional<std::string>{"Register the symbol in NativeBindingRegistry before calling it."}));
  }

  const BindingContext context{
      .host = const_cast<VmHost*>(this),
      .symbol = qualified_symbol,
  };
  return binding->callable(context, args);
}

auto VmHost::call_fleaux(const std::string_view qualified_symbol, const VmValue& args) const -> VmResult {
  if (!impl_->last_fleaux_module.has_value()) {
    return tl::unexpected(make_error(
        HostErrorCategory::kBinding,
        "No Fleaux module is available for call_fleaux.",
        std::optional<std::string>{"Run run_source or run_file before invoking Fleaux symbols."}));
  }

  impl_->set_process_args_for_run(std::string{qualified_symbol});
  std::ostringstream captured_stdout;
  const auto invoked = impl_->runtime.invoke_symbol(*impl_->last_fleaux_module, qualified_symbol, args, captured_stdout);
  impl_->emit_stdout(captured_stdout.str());

  if (!invoked) {
    return tl::unexpected(
        make_error(HostErrorCategory::kRuntime, invoked.error().message, invoked.error().hint, invoked.error().span));
  }
  return *invoked;
}

auto VmHost::call(const std::string_view qualified_symbol, const VmValue& args) const -> VmResult {
  return call_native(qualified_symbol, args);
}

auto VmHost::load_binding_module(const std::filesystem::path& module_path) -> HostStatus {
  if (module_path.empty()) {
    return tl::unexpected(make_error(HostErrorCategory::kBinding,
                                     "Binding module path cannot be empty.",
                                     std::optional<std::string>{"Pass an absolute or relative shared library path."}));
  }

  auto* registry = impl_->config.binding_registry;
  if (registry == nullptr) {
    return tl::unexpected(
        make_error(HostErrorCategory::kBinding,
                   "No binding registry configured for module loading.",
                   std::optional<std::string>{"Set VmHostConfig.binding_registry or call set_binding_registry()."}));
  }

  auto* loader = impl_->dynamic_loader;
  if (loader == nullptr) {
    return tl::unexpected(make_error(HostErrorCategory::kInternal,
                                     "No dynamic loader configured for module loading.",
                                     std::optional<std::string>{"Call set_dynamic_loader with a valid loader."}));
  }

  auto loaded_library = loader->open(module_path);
  if (!loaded_library) {
    return tl::unexpected(make_error(HostErrorCategory::kBinding, loaded_library.error().message, loaded_library.error().hint));
  }

  auto before_symbols = registry->snapshot_symbols();

  const auto entrypoint = loaded_library.value()->symbol(kBindingPluginRegisterEntrypoint);
  if (!entrypoint) {
    return tl::unexpected(make_error(HostErrorCategory::kBinding,
                                     entrypoint.error().message,
                                     std::optional<std::string>{"Binding module is missing the required registration entrypoint."}));
  }

  const auto register_module = reinterpret_cast<RegisterBindingModuleFn>(*entrypoint);
  if (register_module == nullptr) {
    return tl::unexpected(make_error(HostErrorCategory::kBinding,
                                     "Binding module entrypoint pointer is null.",
                                     std::optional<std::string>{"Verify the exported registration function signature."}));
  }

  const BindingPluginHostApi host_api{
      .abi_version = kBindingPluginAbiVersion,
      .registry = registry,
      .host = this,
      .register_callable = &host_register_callable,
  };

  const char* plugin_error = nullptr;
  const auto status = register_module(&host_api, &plugin_error);
  if (status == BindingPluginStatus::kAbiMismatch) {
    return tl::unexpected(make_error(
        HostErrorCategory::kBinding,
        "Binding module ABI version mismatch.",
        std::optional<std::string>{"Rebuild the binding module against the current Fleaux embed headers."}));
  }
  if (status != BindingPluginStatus::kOk) {
    return tl::unexpected(make_error(
        HostErrorCategory::kBinding,
        plugin_error != nullptr ? std::string{plugin_error} : "Binding module registration failed.",
        std::optional<std::string>{"Inspect module registration logic and symbol registration calls."}));
  }

  auto after_symbols = registry->snapshot_symbols();
  std::vector<std::string> registered_symbols;
  registered_symbols.reserve(after_symbols.size());
  for (const auto& symbol : after_symbols) {
    if (std::ranges::find(before_symbols, symbol) == before_symbols.end()) {
      registered_symbols.push_back(symbol);
    }
  }

  impl_->loaded_binding_modules.push_back(Impl::LoadedBindingModule{
      .library = std::move(*loaded_library),
      .registry = registry,
      .registered_symbols = std::move(registered_symbols),
  });
  return {};
}
void VmHost::add_import_root(const std::filesystem::path& root) {
  impl_->config.import_roots.push_back(root);
}
void VmHost::clear_import_roots() { impl_->config.import_roots.clear(); }

void VmHost::set_binding_registry(NativeBindingRegistry* registry) {
  impl_->unload_binding_modules();
  impl_->config.binding_registry = registry;
}

auto VmHost::binding_registry() const -> NativeBindingRegistry* {
  return impl_->config.binding_registry;
}

void VmHost::set_dynamic_loader(std::unique_ptr<DynamicLoader> loader) {
  impl_->unload_binding_modules();
  impl_->owned_dynamic_loader = std::move(loader);
  impl_->dynamic_loader = impl_->owned_dynamic_loader.get();
}

void VmHost::reset() {
  impl_->last_fleaux_module.reset();
}
}  // namespace fleaux::embed
