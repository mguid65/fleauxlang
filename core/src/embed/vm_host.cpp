#include "fleaux/embed/vm_host.hpp"
#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/embed/binding_plugin.hpp"
#include "fleaux/embed/dynamic_loader.hpp"
#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/value.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

[[nodiscard]] auto optional_input_ref(std::istream* input) -> std::optional<std::reference_wrapper<std::istream>> {
  if (input == nullptr) {
    return std::nullopt;
  }
  return std::ref(*input);
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

[[nodiscard]] auto newly_registered_symbols(const std::vector<std::string>& before_symbols,
                                            const std::vector<std::string>& after_symbols) -> std::vector<std::string> {
  std::vector<std::string> registered_symbols;
  registered_symbols.reserve(after_symbols.size());
  for (const auto& symbol : after_symbols) {
    if (std::ranges::find(before_symbols, symbol) == before_symbols.end()) {
      registered_symbols.push_back(symbol);
    }
  }
  return registered_symbols;
}

void rollback_registered_symbols(NativeBindingRegistry& registry, const std::vector<std::string>& symbols) {
  for (const auto& symbol : std::views::reverse(symbols)) {
    (void)registry.unregister_callable(symbol);
  }
}

[[nodiscard]] auto has_fleaux_export(const bytecode::Module& module, const std::string_view qualified_symbol) -> bool {
  return std::ranges::any_of(module.exports, [qualified_symbol](const bytecode::ExportedSymbol& exported_symbol) {
    return exported_symbol.name == qualified_symbol;
  });
}

[[nodiscard]] auto diagnostic_stage_name(const HostErrorCategory category) -> std::string_view {
  switch (category) {
    case HostErrorCategory::kParse:
      return "parse";
    case HostErrorCategory::kAnalysis:
      return "analysis";
    case HostErrorCategory::kBytecode:
      return "bytecode";
    case HostErrorCategory::kRuntime:
      return "runtime";
    case HostErrorCategory::kIo:
      return "io";
    case HostErrorCategory::kBinding:
      return "binding";
    case HostErrorCategory::kInternal:
      return "internal";
  }
  return "internal";
}
}  // namespace

struct VmHost::Impl {
  struct LoadedBindingModule {
    // Retained only to keep the plugin loaded until unregister-before-unload
    // has removed all plugin-owned callables from the registry.
    [[maybe_unused]] std::unique_ptr<DynamicLibrary> library{};
    NativeBindingRegistry* registry{nullptr};
    std::vector<std::string> registered_symbols{};
  };

  explicit Impl(VmHostConfig initial_config)
      : config(std::move(initial_config)), runtime(config.process_args),
        owned_dynamic_loader(make_system_dynamic_loader()) {}
  Impl(Impl&& other) noexcept = default;
  auto operator=(Impl&& other) noexcept -> Impl& = default;

  VmHostConfig config;
  vm::Runtime runtime{};
  std::unique_ptr<DynamicLoader> owned_dynamic_loader{};
  DynamicLoader* dynamic_loader{owned_dynamic_loader.get()};
  std::vector<LoadedBindingModule> loaded_binding_modules{};
  std::optional<bytecode::Module> last_fleaux_module{std::nullopt};

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

  void emit_stderr(const std::string_view text) const {
    if (text.empty()) {
      return;
    }
    if (!config.stderr_sink.has_value()) {
      return;
    }
    (*config.stderr_sink)(text);
  }

  void emit_host_error(const HostError& error) const {
    std::string rendered = frontend::diag::format_diagnostic(std::string{diagnostic_stage_name(error.category)},
                                                             error.message, error.span, error.hint);
    rendered.push_back('\n');
    emit_stderr(rendered);
  }

  template <typename T>
  auto finalize_result(tl::expected<T, HostError> result) const -> tl::expected<T, HostError> {
    if (!result.has_value()) {
      emit_host_error(result.error());
    }
    return std::move(result);
  }

  [[nodiscard]] auto resolve_entry_path(const std::filesystem::path& path) const
      -> std::optional<std::filesystem::path> {
    if (path.is_absolute() && std::filesystem::exists(path)) {
      return std::filesystem::weakly_canonical(path);
    }
    if (std::filesystem::exists(path)) {
      return std::filesystem::weakly_canonical(path);
    }
    for (const auto& root : config.import_roots) {
      if (const auto candidate = root / path; std::filesystem::exists(candidate)) {
        return std::filesystem::weakly_canonical(candidate);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto execute_module(const bytecode::Module& module, const std::string& entry_label) const -> VmResult {
    std::ostringstream captured_stdout;
    const auto exec_result = runtime.execute(module, vm::RuntimeInvocationOptions{
                                                         .entry_label = entry_label,
                                                         .input = optional_input_ref(config.stdin_stream),
                                                         .output = std::ref(captured_stdout),
                                                     });
    emit_stdout(captured_stdout.str());
    if (!exec_result) {
      return tl::unexpected(make_error(HostErrorCategory::kRuntime, exec_result.error().message,
                                       exec_result.error().hint, exec_result.error().span));
    }
    return runtime::make_int(static_cast<runtime::Int>(exec_result->exit_code));
  }

  [[nodiscard]] auto invoke_native(VmHost* host, const std::string_view qualified_symbol, const VmValue& args) const
      -> VmResult {
    const auto* registry = config.binding_registry;
    if (registry == nullptr) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, "No binding registry configured for VmHost.call_native.",
                     std::optional<std::string>{"Set VmHostConfig.binding_registry or call set_binding_registry()."}));
    }

    const auto binding = registry->find_callable(qualified_symbol);
    if (!binding.has_value()) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, "Binding symbol not found: '" + std::string{qualified_symbol} + "'.",
                     std::optional<std::string>{"Register the symbol in NativeBindingRegistry before calling it."}));
    }

    const BindingContext context{
        .host = *host,
        .symbol = qualified_symbol,
    };
    return binding->get().callable(context, args);
  }

  [[nodiscard]] auto invoke_fleaux(const std::string_view qualified_symbol, const VmValue& args) const -> VmResult {
    if (!last_fleaux_module.has_value()) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, "No Fleaux module is available for call_fleaux.",
                     std::optional<std::string>{"Run run_source or run_file before invoking Fleaux symbols."}));
    }

    std::ostringstream captured_stdout;
    const auto invoked = runtime.invoke_symbol(*last_fleaux_module, qualified_symbol, args,
                                               vm::RuntimeInvocationOptions{
                                                   .entry_label = std::string{qualified_symbol},
                                                   .input = optional_input_ref(config.stdin_stream),
                                                   .output = std::ref(captured_stdout),
                                               });
    emit_stdout(captured_stdout.str());

    if (!invoked) {
      return tl::unexpected(
          make_error(HostErrorCategory::kRuntime, invoked.error().message, invoked.error().hint, invoked.error().span));
    }
    return *invoked;
  }

  void unload_binding_modules() {
    for (const auto& module : std::views::reverse(loaded_binding_modules)) {
      if (module.registry != nullptr) {
        for (const auto& symbol : module.registered_symbols) {
          (void)module.registry->unregister_callable(symbol);
        }
      }
    }
    loaded_binding_modules.clear();
  }
};

VmHost::VmHost(const VmHostConfig& config) : impl_(std::in_place, config) {}

VmHost::VmHost(VmHost&& other) noexcept = default;

auto VmHost::operator=(VmHost&& other) noexcept -> VmHost& = default;

VmHost::~VmHost() = default;

auto VmHost::run_file(const std::filesystem::path& path) -> VmResult {
  auto result = [&]() -> VmResult {
    const auto resolved_path = impl_->resolve_entry_path(path);
    if (!resolved_path.has_value()) {
      return tl::unexpected(make_error(HostErrorCategory::kIo, "Failed to locate source file: '" + path.string() + "'.",
                                       std::optional<std::string>{"Check the path or add an import root."}));
    }

    auto module_result = bytecode::load_linked_module(
        *resolved_path, bytecode::ModuleLoadOptions{
                            .mode = bytecode::OptimizationMode::kBaseline,
                            .write_bytecode_cache = true,
                            .enable_experimental_builtin_reductions =
                                impl_->config.compile_options.enable_experimental_builtin_reductions,
                            .enable_value_ref_gate = impl_->config.compile_options.enable_value_ref_gate,
                            .enable_auto_value_ref = impl_->config.compile_options.enable_auto_value_ref,
                            .value_ref_byte_cutoff = impl_->config.compile_options.value_ref_byte_cutoff,
                        });
    if (!module_result) {
      return tl::unexpected(make_error(HostErrorCategory::kBytecode, module_result.error().message));
    }

    impl_->last_fleaux_module = std::move(*module_result);
    return impl_->execute_module(*impl_->last_fleaux_module, resolved_path->string());
  }();

  return impl_->finalize_result(std::move(result));
}

auto VmHost::run_source(const std::string_view module_name, const std::string_view source_text) -> VmResult {
  auto result = [&]() -> VmResult {
    if (module_name.empty()) {
      return tl::unexpected(
          make_error(HostErrorCategory::kIo, "Module name is required for run_source.",
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
      return tl::unexpected(make_error(HostErrorCategory::kAnalysis, lowered.error().message, lowered.error().hint,
                                       lowered.error().span));
    }
    const auto seed_root =
        impl_->config.import_roots.empty() ? std::filesystem::path{} : impl_->config.import_roots.front();
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

    constexpr bytecode::BytecodeCompiler compiler;
    const auto compiled = compiler.compile(*analyzed, compile_options);

    if (!compiled) {
      return tl::unexpected(make_error(HostErrorCategory::kBytecode, compiled.error().message));
    }

    impl_->last_fleaux_module = *compiled;
    return impl_->execute_module(*impl_->last_fleaux_module, std::string{module_name});
  }();

  return impl_->finalize_result(std::move(result));
}

auto VmHost::call_native(const std::string_view qualified_symbol, const VmValue& args) -> VmResult {
  auto result = impl_->invoke_native(this, qualified_symbol, args);
  return impl_->finalize_result(std::move(result));
}

auto VmHost::call_fleaux(const std::string_view qualified_symbol, const VmValue& args) -> VmResult {
  auto result = impl_->invoke_fleaux(qualified_symbol, args);
  return impl_->finalize_result(std::move(result));
}

auto VmHost::call(const std::string_view qualified_symbol, const VmValue& args) -> VmResult {
  auto result = [&]() -> VmResult {
    const bool has_fleaux_symbol =
        impl_->last_fleaux_module.has_value() && has_fleaux_export(*impl_->last_fleaux_module, qualified_symbol);
    const auto* registry = impl_->config.binding_registry;
    const bool has_native_symbol = registry != nullptr && registry->has_callable(qualified_symbol);

    if (has_fleaux_symbol && has_native_symbol) {
      return tl::unexpected(make_error(
          HostErrorCategory::kBinding, "Ambiguous callable symbol: '" + std::string{qualified_symbol} + "'.",
          std::optional<std::string>{"Use call_fleaux() or call_native() to choose the dispatch surface explicitly."}));
    }
    if (has_fleaux_symbol) {
      return impl_->invoke_fleaux(qualified_symbol, args);
    }
    if (has_native_symbol) {
      return impl_->invoke_native(this, qualified_symbol, args);
    }
    if (impl_->last_fleaux_module.has_value() || registry != nullptr) {
      return tl::unexpected(make_error(
          HostErrorCategory::kBinding, "Callable symbol not found: '" + std::string{qualified_symbol} + "'.",
          std::optional<std::string>{"Load a Fleaux module or register a native binding with that symbol, or call the "
                                     "explicit dispatch surface directly."}));
    }
    return tl::unexpected(make_error(
        HostErrorCategory::kBinding, "No Fleaux module or native binding registry is available for VmHost.call().",
        std::optional<std::string>{
            "Run run_source()/run_file() or configure VmHostConfig.binding_registry before calling symbols."}));
  }();

  return impl_->finalize_result(std::move(result));
}

auto VmHost::load_binding_module(const std::filesystem::path& module_path) -> HostStatus {
  auto result = [&]() -> HostStatus {
    if (module_path.empty()) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, "Binding module path cannot be empty.",
                     std::optional<std::string>{"Pass an absolute or relative shared library path."}));
    }

    auto* registry = impl_->config.binding_registry;
    if (registry == nullptr) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, "No binding registry configured for module loading.",
                     std::optional<std::string>{"Set VmHostConfig.binding_registry or call set_binding_registry()."}));
    }

    const auto* loader = impl_->dynamic_loader;
    if (loader == nullptr) {
      return tl::unexpected(make_error(HostErrorCategory::kInternal, "No dynamic loader configured for module loading.",
                                       std::optional<std::string>{"Call set_dynamic_loader with a valid loader."}));
    }

    auto loaded_library = loader->open(module_path);
    if (!loaded_library) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, loaded_library.error().message, loaded_library.error().hint));
    }

    auto before_symbols = registry->snapshot_symbols();

    const auto register_module = resolve_binding_module_entrypoint(**loaded_library);
    if (!register_module) {
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding, register_module.error().message, register_module.error().hint));
    }

    const BindingPluginHostApi host_api{
        .abi_version = kBindingPluginAbiVersion,
        .registry = registry,
        .host = this,
        .register_callable = &host_register_callable,
    };

    const char* plugin_error = nullptr;
    const auto status = (*register_module)(&host_api, &plugin_error);
    const auto rollback_new_symbols = [&registry, &before_symbols]() -> void {
      rollback_registered_symbols(*registry, newly_registered_symbols(before_symbols, registry->snapshot_symbols()));
    };
    if (status == BindingPluginStatus::kAbiMismatch) {
      rollback_new_symbols();
      return tl::unexpected(make_error(
          HostErrorCategory::kBinding, "Binding module ABI version mismatch.",
          std::optional<std::string>{"Rebuild the binding module against the current Fleaux embed headers."}));
    }
    if (status != BindingPluginStatus::kOk) {
      rollback_new_symbols();
      return tl::unexpected(
          make_error(HostErrorCategory::kBinding,
                     plugin_error != nullptr ? std::string{plugin_error} : "Binding module registration failed.",
                     std::optional<std::string>{"Inspect module registration logic and symbol registration calls."}));
    }

    const auto after_symbols = registry->snapshot_symbols();
    auto registered_symbols = newly_registered_symbols(before_symbols, after_symbols);

    impl_->loaded_binding_modules.push_back(Impl::LoadedBindingModule{
        .library = std::move(*loaded_library),
        .registry = registry,
        .registered_symbols = std::move(registered_symbols),
    });
    return {};
  }();

  return impl_->finalize_result(std::move(result));
}
void VmHost::add_import_root(const std::filesystem::path& root) { impl_->config.import_roots.push_back(root); }
void VmHost::clear_import_roots() { impl_->config.import_roots.clear(); }

void VmHost::set_binding_registry(NativeBindingRegistry* registry) {
  impl_->unload_binding_modules();
  impl_->config.binding_registry = registry;
}

auto VmHost::binding_registry() const -> NativeBindingRegistry* { return impl_->config.binding_registry; }

void VmHost::set_dynamic_loader(std::unique_ptr<DynamicLoader> loader) {
  impl_->unload_binding_modules();
  impl_->owned_dynamic_loader = std::move(loader);
  impl_->dynamic_loader = impl_->owned_dynamic_loader.get();
}

void VmHost::reset() { impl_->last_fleaux_module.reset(); }
}  // namespace fleaux::embed
