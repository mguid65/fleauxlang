#include "fleaux/frontend/cpp_transpiler.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

namespace fleaux::frontend::cpp_transpile {

namespace {

std::string read_file(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    return {};
  }

  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string sanitize_symbol(std::string value) {
  for (char& ch : value) {
    const bool is_alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '_';
    if (!is_alnum) {
      ch = '_';
    }
  }
  if (value.empty()) {
    value = "_";
  }
  if (value.front() >= '0' && value.front() <= '9') {
    value.insert(value.begin(), '_');
  }
  return value;
}

std::string symbol_name(const std::optional<std::string>& qualifier, const std::string& name) {
  if (!qualifier.has_value()) {
    return sanitize_symbol(name);
  }
  return sanitize_symbol(*qualifier) + "_" + sanitize_symbol(name);
}

std::string quote_cpp_string(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2U);
  out.push_back('"');
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool is_std_qualified(const std::string& qualifier) {
  return qualifier == "Std" || qualifier.rfind("Std.", 0) == 0;
}

const std::unordered_map<std::string, std::string> kOperatorToBuiltin = {
    {"+", "Add"},
    {"-", "Subtract"},
    {"*", "Multiply"},
    {"/", "Divide"},
    {"%", "Mod"},
    {"^", "Pow"},
    {"==", "Equal"},
    {"!=", "NotEqual"},
    {"<", "LessThan"},
    {">", "GreaterThan"},
    {">=", "GreaterOrEqual"},
    {"<=", "LessOrEqual"},
    {"!", "Not"},
    {"&&", "And"},
    {"||", "Or"},
};

#define FLEAUX_BUILTIN_NODE_STRING(name_literal, node_type) {name_literal, #node_type},
const std::unordered_map<std::string, std::string> kBuiltinNameMap = {
    FLEAUX_VM_BUILTINS(FLEAUX_BUILTIN_NODE_STRING)
};
#undef FLEAUX_BUILTIN_NODE_STRING

#define FLEAUX_CONST_BUILTIN_VALUE(name_literal, numeric_value) {name_literal, numeric_value},
const std::unordered_map<std::string, double> kConstantBuiltins = {
    FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_CONST_BUILTIN_VALUE)
};
#undef FLEAUX_CONST_BUILTIN_VALUE

std::string missing_builtin_expr(const std::string& key) {
  return "_fleaux_missing_builtin(" + quote_cpp_string(key) + ")";
}

std::string builtin_node_expr(const std::string& builtin_key) {
  const auto constant_it = kConstantBuiltins.find(builtin_key);
  if (constant_it != kConstantBuiltins.end()) {
    std::ostringstream val;
    val.precision(std::numeric_limits<double>::max_digits10);
    val << constant_it->second;
    return "_fleaux_constant_builtin(" + val.str() + ")";
  }

  const auto builtin_it = kBuiltinNameMap.find(builtin_key);
  if (builtin_it == kBuiltinNameMap.end()) {
    return missing_builtin_expr(builtin_key);
  }
  return "fleaux::runtime::" + builtin_it->second + "{}";
}

std::string compile_constant(const ir::IRConstant& c) {
  if (std::holds_alternative<std::monostate>(c.val)) {
    return "fleaux::runtime::make_null()";
  }
  if (std::holds_alternative<bool>(c.val)) {
    return std::get<bool>(c.val) ? "fleaux::runtime::make_bool(true)"
                                 : "fleaux::runtime::make_bool(false)";
  }
  if (std::holds_alternative<std::int64_t>(c.val)) {
    return "fleaux::runtime::make_int(" + std::to_string(std::get<std::int64_t>(c.val)) + ")";
  }
  if (std::holds_alternative<double>(c.val)) {
    std::ostringstream out;
    out << "fleaux::runtime::make_float(" << std::get<double>(c.val) << ")";
    return out.str();
  }
  return "fleaux::runtime::make_string(" + quote_cpp_string(std::get<std::string>(c.val)) + ")";
}

std::string compile_name_ref(const ir::IRNameRef& name,
                             const std::unordered_map<std::string, std::string>& local_bindings,
                             const std::unordered_map<std::string, std::string>& known_symbols) {
  if (!name.qualifier.has_value()) {
    const auto local_it = local_bindings.find(name.name);
    if (local_it != local_bindings.end()) {
      return local_it->second;
    }

    const auto known_it = known_symbols.find(name.name);
    if (known_it != known_symbols.end()) {
      return known_it->second;
    }

    return sanitize_symbol(name.name);
  }

  const std::string qualified = *name.qualifier + "." + name.name;
  const auto known_it = known_symbols.find(qualified);
  if (known_it != known_symbols.end()) {
    return known_it->second;
  }

  if (is_std_qualified(*name.qualifier)) {
    return builtin_node_expr(qualified);
  }

  return symbol_name(name.qualifier, name.name);
}

std::string compile_call_target(
    const ir::IRCallTarget& target,
    const std::unordered_map<std::string, std::string>& local_bindings,
    const std::unordered_map<std::string, std::string>& known_symbols) {
  if (std::holds_alternative<ir::IROperatorRef>(target)) {
    const auto& op = std::get<ir::IROperatorRef>(target);
    const auto op_it = kOperatorToBuiltin.find(op.op);
    const std::string builtin = op_it == kOperatorToBuiltin.end() ? op.op : op_it->second;
    return builtin_node_expr("Std." + builtin);
  }

  const auto& name = std::get<ir::IRNameRef>(target);
  if (name.qualifier.has_value() && is_std_qualified(*name.qualifier)) {
    return builtin_node_expr(*name.qualifier + "." + name.name);
  }
  return compile_name_ref(name, local_bindings, known_symbols);
}

std::string compile_expr(const ir::IRExprPtr& expr,
                         const std::unordered_map<std::string, std::string>& local_bindings,
                         const std::unordered_map<std::string, std::string>& known_symbols) {
  if (!expr) {
    return "fleaux::runtime::make_null()";
  }

  if (std::holds_alternative<ir::IRFlowExpr>(expr->node)) {
    const auto& flow = std::get<ir::IRFlowExpr>(expr->node);
    return "(" + compile_expr(flow.lhs, local_bindings, known_symbols) + " | " +
           compile_call_target(flow.rhs, local_bindings, known_symbols) + ")";
  }

  if (std::holds_alternative<ir::IRTupleExpr>(expr->node)) {
    const auto& tuple = std::get<ir::IRTupleExpr>(expr->node);
    std::vector<std::string> parts;
    parts.reserve(tuple.items.size());
    for (const auto& item : tuple.items) {
      if (item && std::holds_alternative<ir::IRNameRef>(item->node)) {
        const auto& name_ref = std::get<ir::IRNameRef>(item->node);
        if (name_ref.qualifier.has_value() || !local_bindings.contains(name_ref.name)) {
          const std::string fn_ref = compile_name_ref(name_ref, local_bindings, known_symbols);
          parts.push_back("fleaux::runtime::make_callable_ref(" + fn_ref + ")");
          continue;
        }
      }
      parts.push_back(compile_expr(item, local_bindings, known_symbols));
    }

    std::ostringstream out;
    out << "fleaux::runtime::make_tuple(";
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i > 0) {
        out << ", ";
      }
      out << parts[i];
    }
    out << ")";
    return out.str();
  }

  if (std::holds_alternative<ir::IRConstant>(expr->node)) {
    return compile_constant(std::get<ir::IRConstant>(expr->node));
  }

  return compile_name_ref(std::get<ir::IRNameRef>(expr->node), local_bindings, known_symbols);
}

std::string emit_let_definition(const ir::IRLet& let,
                                const std::unordered_map<std::string, std::string>& known_symbols) {
  const std::string fn_name = symbol_name(let.qualifier, let.name);
  std::ostringstream out;
  out << "fleaux::runtime::Value " << fn_name << "(fleaux::runtime::Value _fleaux_arg) {\n";

  if (let.is_builtin) {
    std::string builtin_key = let.name;
    if (let.qualifier.has_value()) {
      builtin_key = *let.qualifier + "." + let.name;
    }
    out << "  return (_fleaux_arg | " << builtin_node_expr(builtin_key) << ");\n";
    out << "}\n\n";
    return out.str();
  }

  std::unordered_map<std::string, std::string> local_bindings;
  for (const auto& param : let.params) {
    local_bindings[param.name] = sanitize_symbol(param.name);
  }

  if (let.params.size() == 1U) {
    out << "  fleaux::runtime::Value " << local_bindings[let.params[0].name]
        << " = fleaux::runtime::unwrap_singleton_arg(_fleaux_arg);\n";
  } else if (let.params.size() > 1U) {
    out << "  const fleaux::runtime::Value& _fleaux_args = _fleaux_arg;\n";
    for (std::size_t idx = 0; idx < let.params.size(); ++idx) {
      out << "  fleaux::runtime::Value " << local_bindings[let.params[idx].name]
          << " = fleaux::runtime::array_at(_fleaux_args, " << idx << ");\n";
    }
  }

  out << "  return " << compile_expr(let.body, local_bindings, known_symbols) << ";\n";
  out << "}\n\n";
  return out.str();
}

tl::expected<ir::IRProgram, TranspileError> parse_and_lower(const std::filesystem::path& source_file) {
  const auto source_text = read_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(TranspileError{
        .message = "Failed to read source file.",
        .hint = "Check the file path and ensure it is not empty.",
        .span = std::nullopt,
    });
  }

  const parse::Parser parser;
  const auto parse_result = parser.parse_program(source_text, source_file.string());
  if (!parse_result) {
    return tl::unexpected(TranspileError{
        .message = parse_result.error().message,
        .hint = parse_result.error().hint,
        .span = parse_result.error().span,
    });
  }

  const lowering::Lowerer lowerer;
  const auto lowering_result = lowerer.lower(parse_result.value());
  if (!lowering_result) {
    return tl::unexpected(TranspileError{
        .message = lowering_result.error().message,
        .hint = lowering_result.error().hint,
        .span = lowering_result.error().span,
    });
  }

  return lowering_result.value();
}

std::filesystem::path resolve_import_source(const std::filesystem::path& current_source,
                                            const std::string& module_name) {
  if (module_name == "Std" || module_name == "StdBuiltins") {
    return {};
  }

  const auto local = current_source.parent_path() / (module_name + ".fleaux");
  if (std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  const auto workspace_std = current_source.parent_path().parent_path() / "Std.fleaux";
  if (module_name == "Std" && std::filesystem::exists(workspace_std)) {
    return std::filesystem::weakly_canonical(workspace_std);
  }

  return {};
}

tl::expected<ir::IRProgram, TranspileError> collect_program(
    const std::filesystem::path& source_file,
    std::unordered_map<std::string, ir::IRProgram>& cache,
    std::unordered_set<std::string>& in_progress) {
  const std::string key = std::filesystem::weakly_canonical(source_file).string();
  if (cache.contains(key)) {
    return cache[key];
  }

  if (in_progress.contains(key)) {
    return tl::unexpected(TranspileError{
        .message = "Cyclic import detected while transpiling.",
        .hint = "Break the cycle by moving shared definitions into a third module.",
        .span = std::nullopt,
    });
  }

  in_progress.insert(key);
  auto current_result = parse_and_lower(source_file);
  if (!current_result) {
    in_progress.erase(key);
    return tl::unexpected(current_result.error());
  }

  ir::IRProgram merged = current_result.value();
  std::unordered_set<std::string> seen_symbols;
  for (const auto& let : merged.lets) {
    seen_symbols.insert(symbol_name(let.qualifier, let.name));
  }

  std::vector<ir::IRExprStatement> imported_exprs;
  std::vector<ir::IRLet> imported_lets;

  for (const auto& import_stmt : current_result->imports) {
    const auto import_source = resolve_import_source(source_file, import_stmt.module_name);
    if (import_source.empty()) {
      continue;
    }

    auto imported_result = collect_program(import_source, cache, in_progress);
    if (!imported_result) {
      in_progress.erase(key);
      return tl::unexpected(imported_result.error());
    }

    imported_exprs.insert(imported_exprs.end(), imported_result->expressions.begin(),
                          imported_result->expressions.end());
    for (const auto& imported_let : imported_result->lets) {
      const std::string sym = symbol_name(imported_let.qualifier, imported_let.name);
      if (seen_symbols.insert(sym).second) {
        imported_lets.push_back(imported_let);
      }
    }
  }

  merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

  cache[key] = merged;
  in_progress.erase(key);
  return merged;
}

}  // namespace

TranspileResult FleauxCppTranspiler::process(const std::filesystem::path& source_file) const {
  std::unordered_map<std::string, ir::IRProgram> cache;
  std::unordered_set<std::string> in_progress;

  auto merged_result = collect_program(source_file, cache, in_progress);
  if (!merged_result) {
    return tl::unexpected(merged_result.error());
  }

  const auto module_name = sanitize_symbol(source_file.stem().string());
  const auto output = source_file.parent_path() / ("fleaux_generated_module_" + module_name + ".cpp");

  std::ofstream out(output);
  if (!out) {
    return tl::unexpected(TranspileError{
        .message = "Failed to create output C++ file.",
        .hint = "Verify the destination directory is writable.",
        .span = std::nullopt,
    });
  }

  out << emit_cpp(merged_result.value(), module_name);
  return output;
}

std::string FleauxCppTranspiler::emit_cpp(const ir::IRProgram& program,
                                          const std::string& module_name) const {
  std::unordered_map<std::string, std::string> known_symbols;
  for (const auto& let : program.lets) {
    const std::string symbol = symbol_name(let.qualifier, let.name);
    known_symbols[let.name] = symbol;
    if (let.qualifier.has_value()) {
      known_symbols[*let.qualifier + "." + let.name] = symbol;
    }
  }

  std::ostringstream out;
  out << "#include \"fleaux_runtime.hpp\"\n";
  out << "#include <stdexcept>\n";
  out << "#include <string>\n\n";
  out << "using fleaux::runtime::operator|;\n\n";
  out << "namespace {\n";
  out << "struct _FleauxMissingBuiltin {\n";
  out << "  const char* name;\n";
  out << "  fleaux::runtime::Value operator()(fleaux::runtime::Value) const {\n";
  out << "    throw std::runtime_error(std::string(\"Builtin '\") + name + \"' is not yet implemented in cpp/fleaux_runtime.hpp\");\n";
  out << "  }\n";
  out << "};\n";
  out << "inline _FleauxMissingBuiltin _fleaux_missing_builtin(const char* name) {\n";
  out << "  return _FleauxMissingBuiltin{name};\n";
  out << "}\n";
  out << "struct _FleauxConstantBuiltin {\n";
  out << "  fleaux::runtime::Value value;\n";
  out << "  fleaux::runtime::Value operator()(fleaux::runtime::Value) const {\n";
  out << "    return value;\n";
  out << "  }\n";
  out << "};\n";
  out << "inline _FleauxConstantBuiltin _fleaux_constant_builtin(double value) {\n";
  out << "  return _FleauxConstantBuiltin{fleaux::runtime::make_float(value)};\n";
  out << "}\n";
  out << "}  // namespace\n\n";

  out << "namespace fleaux_gen_" << module_name << " {\n\n";
  out << "fleaux::runtime::Value _fleaux_run_module();\n";
  out << "void _fleaux_init_module();\n";
  out << "extern fleaux::runtime::Value _fleaux_last_value;\n\n";

  for (const auto& let : program.lets) {
    out << "fleaux::runtime::Value " << symbol_name(let.qualifier, let.name)
        << "(fleaux::runtime::Value _fleaux_arg);\n";
  }
  out << "\n";

  for (const auto& let : program.lets) {
    out << emit_let_definition(let, known_symbols);
  }

  out << "fleaux::runtime::Value _fleaux_run_module() {\n";
  out << "  fleaux::runtime::Value _fleaux_last_value = fleaux::runtime::make_null();\n";
  std::unordered_map<std::string, std::string> no_locals;
  for (const auto& expr_stmt : program.expressions) {
    out << "  _fleaux_last_value = "
        << compile_expr(expr_stmt.expr, no_locals, known_symbols) << ";\n";
  }
  out << "  return _fleaux_last_value;\n";
  out << "}\n\n";
  out << "fleaux::runtime::Value _fleaux_last_value = fleaux::runtime::make_null();\n";
  out << "void _fleaux_init_module() {\n";
  out << "  _fleaux_last_value = _fleaux_run_module();\n";
  out << "}\n\n";
  out << "}  // namespace fleaux_gen_" << module_name << "\n\n";

  out << "int main(int argc, char** argv) {\n";
  out << "  fleaux::runtime::set_process_args(argc, argv);\n";
  out << "  fleaux_gen_" << module_name << "::_fleaux_init_module();\n";
  out << "  return 0;\n";
  out << "}\n";

  return out.str();
}

}  // namespace fleaux::frontend::cpp_transpile

