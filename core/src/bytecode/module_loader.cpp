#include "fleaux/bytecode/module_loader.hpp"

#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/frontend/source_loader.hpp"

namespace fleaux::bytecode {
namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct ResolvedModulePaths {
  std::optional<std::filesystem::path> source;
  std::optional<std::filesystem::path> bytecode;
};

struct LoaderState {
  std::unordered_map<std::string, Module> unlinked_cache;
  std::unordered_set<std::string> unlinked_in_progress;
};

struct LinkContext {
  Module linked_module;
  std::unordered_map<std::string, std::uint32_t> resolved_function_indices;
  std::unordered_set<std::string> merged_module_keys;
  std::unordered_set<std::string> in_progress;
  std::string entry_key;
};

struct MergeMaps {
  std::vector<std::uint32_t> constants;
  std::vector<std::uint32_t> builtins;
  std::vector<std::uint32_t> functions;
  std::vector<std::uint32_t> closures;
};

auto make_error(const std::string& message) -> ModuleLoadError {
  return ModuleLoadError{.message = message};
}

auto hash_text(const std::string& text) -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

auto path_exists(const std::filesystem::path& path) -> bool {
  return !path.empty() && std::filesystem::exists(path);
}

auto canonical_if_exists(const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
  if (!path_exists(path)) { return std::nullopt; }
  return std::filesystem::weakly_canonical(path);
}

auto normalized_path(const std::filesystem::path& path) -> std::filesystem::path {
  return std::filesystem::absolute(path).lexically_normal();
}

auto is_symbolic_import(const std::string& module_name) -> bool {
  return module_name == "Std" || module_name == "StdBuiltins";
}

auto bytecode_path_for_source(const std::filesystem::path& source_path) -> std::filesystem::path {
  auto bytecode_path = source_path;
  bytecode_path += ".bc";
  return bytecode_path;
}

auto source_path_for_bytecode(const std::filesystem::path& bytecode_path) -> std::filesystem::path {
  return bytecode_path.parent_path() / bytecode_path.stem();
}

auto module_key_for(const ResolvedModulePaths& paths) -> std::string {
  if (paths.source.has_value()) { return paths.source->string(); }
  if (paths.bytecode.has_value()) { return paths.bytecode->string(); }
  return {};
}

auto resolve_entry_paths(const std::filesystem::path& entry_path) -> ResolvedModulePaths {
  ResolvedModulePaths paths;
  if (entry_path.extension() == ".bc") {
    paths.bytecode = canonical_if_exists(entry_path);
    const auto source_candidate = source_path_for_bytecode(entry_path);
    paths.source = canonical_if_exists(source_candidate);
    if (!paths.bytecode.has_value() && path_exists(entry_path)) {
      paths.bytecode = std::filesystem::weakly_canonical(entry_path);
    }
    return paths;
  }

  paths.source = canonical_if_exists(entry_path);
  const auto bytecode_candidate = bytecode_path_for_source(entry_path);
  if (paths.source.has_value()) {
    paths.bytecode = canonical_if_exists(bytecode_candidate).value_or(normalized_path(bytecode_candidate));
  } else {
    paths.bytecode = canonical_if_exists(bytecode_candidate);
  }
  return paths;
}

auto resolve_import_paths(const std::filesystem::path& current_module_dir, const std::string& module_name)
    -> ResolvedModulePaths {
  ResolvedModulePaths paths;
  if (is_symbolic_import(module_name)) { return paths; }

  const auto source_candidate = current_module_dir / (module_name + ".fleaux");
  const auto bytecode_candidate = current_module_dir / (module_name + ".fleaux.bc");
  paths.source = canonical_if_exists(source_candidate);
  if (paths.source.has_value()) {
    paths.bytecode = canonical_if_exists(bytecode_candidate).value_or(normalized_path(bytecode_candidate));
  } else {
    paths.bytecode = canonical_if_exists(bytecode_candidate);
  }
  return paths;
}

auto read_binary_file(const std::filesystem::path& file_path) -> tl::expected<std::vector<std::uint8_t>, ModuleLoadError> {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) { return tl::unexpected(make_error("Failed to read bytecode file: " + file_path.string())); }
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_binary_file(const std::filesystem::path& file_path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) { return; }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

auto rebase_instruction_stream(const std::vector<Instruction>& source_instructions, const MergeMaps& maps,
                               const std::int64_t jump_base, const bool replace_terminal_halt_with_noop)
    -> std::vector<Instruction> {
  std::vector<Instruction> rebased = source_instructions;
  if (replace_terminal_halt_with_noop && !rebased.empty() && rebased.back().opcode == Opcode::kHalt) {
    rebased.back().opcode = Opcode::kNoOp;
    rebased.back().operand = 0;
  }

  for (auto& instruction : rebased) {
    switch (instruction.opcode) {
      case Opcode::kPushConst:
        instruction.operand = static_cast<std::int64_t>(maps.constants[static_cast<std::size_t>(instruction.operand)]);
        break;
      case Opcode::kCallBuiltin:
      case Opcode::kMakeBuiltinFuncRef:
        instruction.operand = static_cast<std::int64_t>(maps.builtins[static_cast<std::size_t>(instruction.operand)]);
        break;
      case Opcode::kCallUserFunc:
      case Opcode::kMakeUserFuncRef:
        instruction.operand = static_cast<std::int64_t>(maps.functions[static_cast<std::size_t>(instruction.operand)]);
        break;
      case Opcode::kMakeClosureRef:
        instruction.operand = static_cast<std::int64_t>(maps.closures[static_cast<std::size_t>(instruction.operand)]);
        break;
      case Opcode::kJump:
      case Opcode::kJumpIf:
      case Opcode::kJumpIfNot:
        instruction.operand += jump_base;
        break;
      default:
        break;
    }
  }

  return rebased;
}

auto merge_module_into(Module& target, const Module& source,
                       const std::unordered_map<std::string, std::uint32_t>& resolved_function_indices,
                       const bool keep_exports, const bool keep_terminal_halt)
    -> tl::expected<MergeMaps, ModuleLoadError> {
  MergeMaps maps;
  maps.constants.reserve(source.constants.size());
  for (const auto& constant : source.constants) {
    maps.constants.push_back(static_cast<std::uint32_t>(target.constants.size()));
    target.constants.push_back(constant);
  }

  std::unordered_map<std::string, std::uint32_t> builtin_index_by_name;
  for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(target.builtin_names.size()); ++index) {
    builtin_index_by_name.emplace(target.builtin_names[index], index);
  }
  maps.builtins.reserve(source.builtin_names.size());
  for (const auto& builtin_name : source.builtin_names) {
    if (const auto it = builtin_index_by_name.find(builtin_name); it != builtin_index_by_name.end()) {
      maps.builtins.push_back(it->second);
      continue;
    }
    const auto new_index = static_cast<std::uint32_t>(target.builtin_names.size());
    target.builtin_names.push_back(builtin_name);
    builtin_index_by_name.emplace(builtin_name, new_index);
    maps.builtins.push_back(new_index);
  }

  maps.functions.resize(source.functions.size());
  for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(source.functions.size()); ++index) {
    const auto& function = source.functions[index];
    if (function.is_import_placeholder) {
      const auto resolved = resolved_function_indices.find(function.name);
      if (resolved == resolved_function_indices.end()) {
        return tl::unexpected(make_error("Unresolved imported function: '" + function.name + "'."));
      }
      maps.functions[index] = resolved->second;
      continue;
    }

    maps.functions[index] = static_cast<std::uint32_t>(target.functions.size());
    target.functions.push_back(FunctionDef{
        .name = function.name,
        .arity = function.arity,
        .has_variadic_tail = function.has_variadic_tail,
        .is_import_placeholder = false,
        .instructions = {},
    });
  }

  maps.closures.reserve(source.closures.size());
  for (const auto& closure : source.closures) {
    maps.closures.push_back(static_cast<std::uint32_t>(target.closures.size()));
    target.closures.push_back(ClosureDef{
        .function_index = maps.functions[closure.function_index],
        .capture_count = closure.capture_count,
        .declared_arity = closure.declared_arity,
        .declared_has_variadic_tail = closure.declared_has_variadic_tail,
    });
  }

  for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(source.functions.size()); ++index) {
    const auto& function = source.functions[index];
    if (function.is_import_placeholder) { continue; }
    target.functions[maps.functions[index]].instructions =
        rebase_instruction_stream(function.instructions, maps, 0, false);
  }

  const auto jump_base = static_cast<std::int64_t>(target.instructions.size());
  const auto rebased_top_level = rebase_instruction_stream(source.instructions, maps, jump_base, !keep_terminal_halt);
  target.instructions.insert(target.instructions.end(), rebased_top_level.begin(), rebased_top_level.end());

  if (keep_exports) {
    for (const auto& exported_symbol : source.exports) {
      ExportedSymbol remapped = exported_symbol;
      if (remapped.kind == ExportKind::kFunction) {
        remapped.index = maps.functions[remapped.index];
      }
      target.exports.push_back(std::move(remapped));
    }
  }

  return maps;
}

auto load_unlinked_module(const ResolvedModulePaths& paths, const ModuleLoadOptions& options, LoaderState& state)
    -> tl::expected<Module, ModuleLoadError>;

auto link_module_into(const ResolvedModulePaths& paths, const ModuleLoadOptions& options, LoaderState& state,
                      LinkContext& context) -> tl::expected<void, ModuleLoadError>;

auto load_unlinked_module(const ResolvedModulePaths& paths, const ModuleLoadOptions& options, LoaderState& state)
    -> tl::expected<Module, ModuleLoadError> {
  const std::string key = module_key_for(paths);
  if (key.empty()) { return tl::unexpected(make_error("Unable to resolve module path.")); }
  if (const auto it = state.unlinked_cache.find(key); it != state.unlinked_cache.end()) { return it->second; }
  if (!state.unlinked_in_progress.insert(key).second) {
    return tl::unexpected(make_error("Cyclic bytecode import detected."));
  }

  const auto finish = [&](tl::expected<Module, ModuleLoadError> result) -> tl::expected<Module, ModuleLoadError> {
    state.unlinked_in_progress.erase(key);
    if (result) { state.unlinked_cache.emplace(key, result.value()); }
    return result;
  };

  if (paths.bytecode.has_value()) {
    const auto serialized = read_binary_file(*paths.bytecode);
    if (serialized) {
      const auto deserialized = deserialize_module(*serialized);
      if (deserialized) {
        if (!paths.source.has_value()) {
          return finish(deserialized.value());
        }
        const auto source_text = fleaux::frontend::source_loader::read_text_file(*paths.source);
        if (!source_text.empty() && deserialized->header.source_hash == hash_text(source_text) &&
            deserialized->header.optimization_mode == static_cast<std::uint8_t>(options.mode)) {
          return finish(deserialized.value());
        }
      } else if (!paths.source.has_value()) {
        return finish(tl::unexpected(make_error(deserialized.error().message)));
      }
    } else if (!paths.source.has_value()) {
      return finish(tl::unexpected(serialized.error()));
    }
  }

  if (!paths.source.has_value()) {
    return finish(tl::unexpected(make_error("Module source and bytecode were both unavailable.")));
  }

  const auto source_text = fleaux::frontend::source_loader::read_text_file(*paths.source);
  if (source_text.empty()) {
    return finish(tl::unexpected(make_error("Failed to read source file: " + paths.source->string())));
  }

  const auto make_parse_error = [](const std::string& message, const std::optional<std::string>& hint,
                                   const std::optional<fleaux::frontend::diag::SourceSpan>&) -> ModuleLoadError {
    return ModuleLoadError{.message = hint.has_value() ? message + " (" + *hint + ")" : message};
  };
  const auto ir_program = fleaux::frontend::source_loader::parse_text_to_ir<ModuleLoadError>(
      source_text, paths.source->string(), make_parse_error);
  if (!ir_program) { return finish(tl::unexpected(ir_program.error())); }

  std::vector<Module> imported_modules;
  imported_modules.reserve(ir_program->imports.size());
  for (const auto& [module_name, _span] : ir_program->imports) {
    if (is_symbolic_import(module_name)) { continue; }
    const auto import_paths = resolve_import_paths(paths.source->parent_path(), module_name);
    if (!import_paths.source.has_value() && !import_paths.bytecode.has_value()) {
      return finish(tl::unexpected(make_error("Failed to resolve imported module: " + module_name)));
    }
    auto imported_module = load_unlinked_module(import_paths, options, state);
    if (!imported_module) { return finish(tl::unexpected(imported_module.error())); }
    imported_modules.push_back(std::move(imported_module.value()));
  }

  std::vector<const Module*> imported_module_ptrs;
  imported_module_ptrs.reserve(imported_modules.size());
  for (const auto& imported_module : imported_modules) { imported_module_ptrs.push_back(&imported_module); }

  constexpr BytecodeCompiler compiler;
  auto compiled = compiler.compile(*ir_program, CompileOptions{
                                                    .source_path = *paths.source,
                                                    .source_text = source_text,
                                                    .module_name = paths.source->stem().string(),
                                                    .imported_modules = imported_module_ptrs,
                                                });
  if (!compiled) { return finish(tl::unexpected(make_error(compiled.error().message))); }

  Module module = std::move(compiled.value());
  module.header.optimization_mode = static_cast<std::uint8_t>(options.mode);
  constexpr BytecodeOptimizer optimizer;
  if (const auto optimized = optimizer.optimize(module, OptimizerConfig{.mode = options.mode}); !optimized) {
    return finish(tl::unexpected(make_error(optimized.error().message)));
  }

  if (options.write_bytecode_cache && paths.bytecode.has_value()) {
    if (const auto serialized = serialize_module(module); serialized) {
      write_binary_file(*paths.bytecode, *serialized);
    }
  }

  return finish(module);
}

auto link_module_into(const ResolvedModulePaths& paths, const ModuleLoadOptions& options, LoaderState& state,
                      LinkContext& context) -> tl::expected<void, ModuleLoadError> {
  const std::string key = module_key_for(paths);
  if (key.empty()) { return tl::unexpected(make_error("Unable to resolve module path.")); }
  if (context.merged_module_keys.contains(key)) { return {}; }
  if (!context.in_progress.insert(key).second) {
    return tl::unexpected(make_error("Cyclic bytecode import detected during linking."));
  }

  const auto finish = [&](tl::expected<void, ModuleLoadError> result) -> tl::expected<void, ModuleLoadError> {
    context.in_progress.erase(key);
    if (result) { context.merged_module_keys.insert(key); }
    return result;
  };

  auto unlinked = load_unlinked_module(paths, options, state);
  if (!unlinked) { return finish(tl::unexpected(unlinked.error())); }

  const auto current_dir = (paths.source.has_value() ? paths.source->parent_path() : paths.bytecode->parent_path());
  for (const auto& dependency : unlinked->dependencies) {
    if (dependency.is_symbolic) { continue; }
    const auto dependency_paths = resolve_import_paths(current_dir, dependency.module_name);
    if (!dependency_paths.source.has_value() && !dependency_paths.bytecode.has_value()) {
      return finish(tl::unexpected(make_error("Failed to resolve dependency during linking: " + dependency.module_name)));
    }

    if (auto result = link_module_into(dependency_paths, options, state, context); !result) {
      return finish(tl::unexpected(result.error()));
    }
  }

  const auto merged_current = merge_module_into(context.linked_module, *unlinked, context.resolved_function_indices,
                                                key == context.entry_key, key == context.entry_key);
  if (!merged_current) { return finish(tl::unexpected(merged_current.error())); }

  for (const auto& exported_symbol : unlinked->exports) {
    if (exported_symbol.kind == ExportKind::kFunction) {
      context.resolved_function_indices.emplace(exported_symbol.name, merged_current->functions[exported_symbol.index]);
    }
  }

  return finish({});
}

}  // namespace

auto load_linked_module(const std::filesystem::path& entry_path, const ModuleLoadOptions& options)
    -> tl::expected<Module, ModuleLoadError> {
  const auto paths = resolve_entry_paths(entry_path);
  if (!paths.source.has_value() && !paths.bytecode.has_value()) {
    return tl::unexpected(make_error("Failed to resolve entry module: " + entry_path.string()));
  }

  LoaderState state;
  auto entry_unlinked = load_unlinked_module(paths, options, state);
  if (!entry_unlinked) { return tl::unexpected(entry_unlinked.error()); }

  LinkContext context;
  context.entry_key = module_key_for(paths);
  if (auto result = link_module_into(paths, options, state, context); !result) {
    return tl::unexpected(result.error());
  }

  context.linked_module.header = entry_unlinked->header;
  context.linked_module.dependencies = entry_unlinked->dependencies;
  context.linked_module.header.payload_checksum = 0;
  return context.linked_module;
}

}  // namespace fleaux::bytecode





