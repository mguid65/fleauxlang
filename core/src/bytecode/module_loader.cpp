#include "fleaux/bytecode/module_loader.hpp"

#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/frontend/import_resolution.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/frontend/type_check.hpp"

namespace fleaux::bytecode {
namespace {
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

using ResolvedModulePaths = fleaux::frontend::import_resolution::ResolvedModulePaths;

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
  std::vector<std::uint32_t> functions;
  std::vector<std::uint32_t> closures;
};

auto make_error(const std::string& message) -> ModuleLoadError { return ModuleLoadError{.message = message}; }

auto hash_text(const std::string& text) -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

auto value_ref_cache_compatible(const ModuleLoadOptions& options) -> bool {
  return !options.enable_value_ref_gate && !options.enable_auto_value_ref;
}

auto full_symbol_name(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto let_identity_key(const fleaux::frontend::ir::IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) {
    return let.symbol_key;
  }
  return full_symbol_name(let.qualifier, let.name);
}

auto export_identity_key(const ExportedSymbol& symbol) -> std::string {
  return symbol.link_name.empty() ? symbol.name : symbol.link_name;
}

auto resolve_entry_paths(const std::filesystem::path& entry_path) -> ResolvedModulePaths {
  return fleaux::frontend::import_resolution::resolve_entry_paths(entry_path);
}

auto resolve_import_paths(const std::filesystem::path& current_module_dir, const std::string& module_name)
    -> ResolvedModulePaths {
  return fleaux::frontend::import_resolution::resolve_import_paths(current_module_dir, module_name);
}

auto read_binary_file(const std::filesystem::path& file_path)
    -> tl::expected<std::vector<std::uint8_t>, ModuleLoadError> {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    return tl::unexpected(make_error("Failed to read bytecode file: " + file_path.string()));
  }
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_binary_file(const std::filesystem::path& file_path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return;
  }
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

  for (auto& [opcode, operand] : rebased) {
    switch (opcode) {
      case Opcode::kPushConst:
        operand = static_cast<std::int64_t>(maps.constants[static_cast<std::size_t>(operand)]);
        break;
      case Opcode::kCallBuiltin:
      case Opcode::kMakeBuiltinFuncRef:
        break;
      case Opcode::kCallUserFunc:
      case Opcode::kMakeUserFuncRef:
        operand = static_cast<std::int64_t>(maps.functions[static_cast<std::size_t>(operand)]);
        break;
      case Opcode::kMakeClosureRef:
        operand = static_cast<std::int64_t>(maps.closures[static_cast<std::size_t>(operand)]);
        break;
      case Opcode::kJump:
      case Opcode::kJumpIf:
      case Opcode::kJumpIfNot:
        operand += jump_base;
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
  for (const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] : source.closures) {
    maps.closures.push_back(static_cast<std::uint32_t>(target.closures.size()));
    target.closures.push_back(ClosureDef{
        .function_index = maps.functions[function_index],
        .capture_count = capture_count,
        .declared_arity = declared_arity,
        .declared_has_variadic_tail = declared_has_variadic_tail,
    });
  }

  for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(source.functions.size()); ++index) {
    const auto& function = source.functions[index];
    if (function.is_import_placeholder) {
      continue;
    }
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
  const std::string key = fleaux::frontend::import_resolution::module_key_for(paths);
  if (key.empty()) {
    return tl::unexpected(make_error("Unable to resolve module path."));
  }
  if (const auto it = state.unlinked_cache.find(key); it != state.unlinked_cache.end()) {
    return it->second;
  }
  if (!state.unlinked_in_progress.insert(key).second) {
    return tl::unexpected(make_error("import-cycle: Import cycle detected involving '" + key + "'"));
  }

  const auto finish = [&](tl::expected<Module, ModuleLoadError> result) -> tl::expected<Module, ModuleLoadError> {
    state.unlinked_in_progress.erase(key);
    if (result) {
      state.unlinked_cache.emplace(key, result.value());
    }
    return result;
  };

  if (paths.bytecode.has_value() && (!paths.source.has_value() || value_ref_cache_compatible(options))) {
    if (const auto serialized = read_binary_file(*paths.bytecode)) {
      if (const auto deserialized = deserialize_module(*serialized)) {
        if (!paths.source.has_value()) {
          return finish(deserialized.value());
        }

        if (const auto source_text = fleaux::frontend::source_loader::read_text_file(*paths.source);
            !source_text.empty() && deserialized->header.source_hash == hash_text(source_text) &&
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
  const auto ir_program = fleaux::frontend::source_loader::parse_text_to_lowered_ir<ModuleLoadError>(
      source_text, paths.source->string(), make_parse_error);
  if (!ir_program) {
    return finish(tl::unexpected(ir_program.error()));
  }

  std::vector<Module> imported_modules;
  std::vector<ResolvedModulePaths> imported_module_paths;

  imported_modules.reserve(ir_program->imports.size());
  imported_module_paths.reserve(ir_program->imports.size());

  for (const auto& [module_name, src_span] : ir_program->imports) {
    if (fleaux::frontend::import_resolution::is_symbolic_import(module_name)) {
      continue;
    }

    const auto import_paths = resolve_import_paths(paths.source->parent_path(), module_name);
    if (!import_paths.source.has_value() && !import_paths.bytecode.has_value()) {
      return finish(tl::unexpected(make_error("import-unresolved: Import not found: '" + module_name +
                                              "'. Checked relative to '" + paths.source->string() +
                                              "'. Verify module name and file location.")));
    }

    auto imported_module = load_unlinked_module(import_paths, options, state);
    if (!imported_module) {
      return finish(tl::unexpected(imported_module.error()));
    }

    imported_module_paths.push_back(import_paths);
    imported_modules.push_back(std::move(imported_module.value()));
  }

  std::unordered_set<std::string> imported_symbols;
  std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets;
  std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls;
  std::vector<fleaux::frontend::ir::IRAliasDecl> imported_alias_decls;
  std::unordered_set<std::string> imported_typed_let_keys;
  std::unordered_set<std::string> imported_alias_decl_keys;

  for (std::size_t idx = 0; idx < imported_modules.size(); ++idx) {
    const auto& imported_module = imported_modules[idx];
    for (const auto& exported : imported_module.exports) {
      if (exported.name.empty()) {
        continue;
      }
      // Track exported symbols exactly as declared by imported modules.
      imported_symbols.insert(exported.name);
    }

    if (idx < imported_module_paths.size()) {
      const auto& [source, bytecode] = imported_module_paths[idx];
      if (!source.has_value()) {
        // No source available – fall back to reconstructing typed-import stubs
        // directly from bytecode metadata.  Only generic functions carry enough
        // information to rebuild a meaningful stub; non-generic functions are
        // skipped (the symbol is already registered in `imported_symbols`, so
        // the type checker simply treats calls to them as Any-typed).
        //
        // Build a map from link_name → FunctionDef for quick lookup.
        std::unordered_map<std::string, const FunctionDef*> fn_by_link_name;
        for (const auto& fn : imported_module.functions) {
          fn_by_link_name.emplace(fn.name, &fn);
        }

        for (const auto& exported : imported_module.exports) {
          if (exported.kind != ExportKind::kFunction) {
            continue;
          }
          const auto id_key = export_identity_key(exported);
          if (!imported_typed_let_keys.insert(id_key).second) {
            continue;  // already seeded by an earlier import with source
          }

          const auto fn_it = fn_by_link_name.find(id_key);
          if (fn_it == fn_by_link_name.end()) {
            continue;
          }
          const FunctionDef& fn = *fn_it->second;
          if (fn.generic_params.empty()) {
            continue;  // non-generic: no stub needed
          }

          // Decompose "Qualifier.Name" → qualifier + name.
          fleaux::frontend::ir::IRLet stub;
          const auto& full_name = exported.name;
          if (const auto dot = full_name.rfind('.'); dot != std::string::npos) {
            stub.qualifier = full_name.substr(0, dot);
            stub.name     = full_name.substr(dot + 1);
          } else {
            stub.name = full_name;
          }
          stub.symbol_key    = id_key;
          stub.generic_params = fn.generic_params;

          // Reconstruct parameter list from the stored type names.
          // For simple/named types (including TypeVar names like "T") this
          // gives a fully accurate IRParam.  Complex structural types (whose
          // outer name is e.g. "Function" or "Tuple") are reconstructed with
          // the outer name only; inner TypeVar bindings are lost but the
          // resulting type is treated permissively by the checker.
          const bool have_type_names = fn.param_type_names.size() == static_cast<std::size_t>(fn.arity);
          for (std::uint32_t p = 0; p < fn.arity; ++p) {
            fleaux::frontend::ir::IRParam param;
            param.name = "_p" + std::to_string(p);
            param.type.name = have_type_names ? fn.param_type_names[p] : "Any";
            param.type.variadic = (fn.has_variadic_tail && p + 1 == fn.arity);
            stub.params.push_back(std::move(param));
          }
          stub.return_type.name = fn.return_type_name.empty() ? "Any" : fn.return_type_name;

          imported_typed_lets.push_back(std::move(stub));
        }
        continue;
      }

      const auto make_import_parse_error =
          [](const std::string& message, const std::optional<std::string>& hint,
             const std::optional<fleaux::frontend::diag::SourceSpan>&) -> ModuleLoadError {
        return ModuleLoadError{.message = hint.has_value() ? message + " (" + *hint + ")" : message};
      };

      const auto imported_ir =
          fleaux::frontend::source_loader::load_ir_program<ModuleLoadError>(*source, make_import_parse_error);
      if (!imported_ir) {
        return finish(tl::unexpected(make_error("Failed to seed typed imports from source: " + source->string() + " (" +
                                                imported_ir.error().message + ")")));
      }

      std::unordered_set<std::string> exported_names;
      std::unordered_set<std::string> exported_keys;
      for (const auto& exported : imported_module.exports) {
        exported_names.insert(exported.name);
        exported_keys.insert(export_identity_key(exported));
      }

      std::unordered_set<std::string> seeded_export_names;

      for (const auto& imported_let : imported_ir->lets) {
        const auto symbol = let_identity_key(imported_let);
        if (!exported_keys.contains(symbol)) {
          continue;
        }
        if (!imported_typed_let_keys.insert(symbol).second) {
          continue;
        }
        seeded_export_names.insert(symbol);
        imported_typed_lets.push_back(imported_let);
      }

      for (const auto& imported_type_decl : imported_ir->type_decls) {
        imported_type_decls.push_back(imported_type_decl);
      }

      for (const auto& imported_alias_decl : imported_ir->alias_decls) {
        if (const auto key = fleaux::frontend::source_loader::alias_decl_identity_key(imported_alias_decl);
            imported_alias_decl_keys.insert(key).second) {
          imported_alias_decls.push_back(imported_alias_decl);
        }
      }

      for (const auto& exported_key : exported_keys) {
        if (seeded_export_names.contains(exported_key)) {
          continue;
        }
        return finish(tl::unexpected(make_error("Failed to seed typed imports from source: " + source->string() +
                                                " (Missing exported declaration for typed import seed: '" +
                                                exported_key + "'.)")));
      }
    }
  }

  if (auto symbolic_seed = fleaux::frontend::source_loader::seed_symbolic_imports_for_program<ModuleLoadError>(
          *ir_program, make_parse_error, imported_symbols, imported_typed_lets, imported_type_decls,
          imported_alias_decls);
      !symbolic_seed) {
    return finish(tl::unexpected(symbolic_seed.error()));
  }

  const auto analyzed =
      fleaux::frontend::type_check::analyze_program(*ir_program, imported_symbols, imported_typed_lets,
                                                    imported_type_decls, imported_alias_decls);
  if (!analyzed) {
    return finish(tl::unexpected(make_error(analyzed.error().hint.has_value()
                                                ? analyzed.error().message + " (" + *analyzed.error().hint + ")"
                                                : analyzed.error().message)));
  }

  constexpr BytecodeCompiler compiler;
  auto compiled = compiler.compile(*analyzed, CompileOptions{
                                                  .source_path = *paths.source,
                                                  .source_text = source_text,
                                                  .module_name = paths.source->stem().string(),
                                                  .imported_modules = std::move(imported_modules),
                                                  .enable_value_ref_gate =
                                                      options.enable_value_ref_gate || options.enable_auto_value_ref,
                                                  .enable_auto_value_ref = options.enable_auto_value_ref,
                                                  .value_ref_byte_cutoff = options.value_ref_byte_cutoff,
                                              });
  if (!compiled) {
    return finish(tl::unexpected(make_error(compiled.error().message)));
  }

  Module mod = std::move(compiled.value());
  mod.header.optimization_mode = static_cast<std::uint8_t>(options.mode);
  constexpr BytecodeOptimizer optimizer;
  if (const auto optimized = optimizer.optimize(mod, OptimizerConfig{.mode = options.mode}); !optimized) {
    return finish(tl::unexpected(make_error(optimized.error().message)));
  }

  if (options.write_bytecode_cache && paths.bytecode.has_value() && value_ref_cache_compatible(options)) {
    if (const auto serialized = serialize_module(mod); serialized) {
      write_binary_file(*paths.bytecode, *serialized);
    }
  }

  return finish(mod);
}

auto link_module_into(const ResolvedModulePaths& paths, const ModuleLoadOptions& options, LoaderState& state,
                      LinkContext& context) -> tl::expected<void, ModuleLoadError> {
  const std::string key = fleaux::frontend::import_resolution::module_key_for(paths);
  if (key.empty()) {
    return tl::unexpected(make_error("Unable to resolve module path."));
  }
  if (context.merged_module_keys.contains(key)) {
    return {};
  }
  if (!context.in_progress.insert(key).second) {
    return tl::unexpected(make_error("import-cycle: Import cycle detected involving '" + key + "'"));
  }

  const auto finish = [&](tl::expected<void, ModuleLoadError> result) -> tl::expected<void, ModuleLoadError> {
    context.in_progress.erase(key);
    if (result) {
      context.merged_module_keys.insert(key);
    }
    return result;
  };

  auto unlinked = load_unlinked_module(paths, options, state);
  if (!unlinked) {
    return finish(tl::unexpected(unlinked.error()));
  }

  const auto current_dir = (paths.source.has_value() ? paths.source->parent_path() : paths.bytecode->parent_path());
  for (const auto& [module_name, is_symbolic] : unlinked->dependencies) {
    if (is_symbolic) {
      continue;
    }
    const auto dependency_paths = resolve_import_paths(current_dir, module_name);
    if (!dependency_paths.source.has_value() && !dependency_paths.bytecode.has_value()) {
      return finish(tl::unexpected(make_error("import-unresolved: Import not found: '" + module_name +
                                              "'. Checked relative to '" + current_dir.string() +
                                              "'. Verify module name and file location.")));
    }

    if (auto result = link_module_into(dependency_paths, options, state, context); !result) {
      return finish(tl::unexpected(result.error()));
    }
  }

  const auto merged_current = merge_module_into(context.linked_module, *unlinked, context.resolved_function_indices,
                                                key == context.entry_key, key == context.entry_key);
  if (!merged_current) {
    return finish(tl::unexpected(merged_current.error()));
  }

  for (const auto& exported_symbol : unlinked->exports) {
    if (exported_symbol.kind == ExportKind::kFunction) {
      context.resolved_function_indices.emplace(export_identity_key(exported_symbol),
                                                merged_current->functions[exported_symbol.index]);
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
  if (!entry_unlinked) {
    return tl::unexpected(entry_unlinked.error());
  }

  LinkContext context;
  context.entry_key = fleaux::frontend::import_resolution::module_key_for(paths);
  if (auto result = link_module_into(paths, options, state, context); !result) {
    return tl::unexpected(result.error());
  }

  context.linked_module.header = entry_unlinked->header;
  context.linked_module.dependencies = entry_unlinked->dependencies;
  context.linked_module.header.payload_checksum = 0;
  return context.linked_module;
}
}  // namespace fleaux::bytecode
