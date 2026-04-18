#include "fleaux/frontend/cpp_transpiler.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

namespace fleaux::frontend::cpp_transpile {

namespace {

auto sanitize_symbol(std::string value) -> std::string {
  for (char& ch : value) {
    const bool is_alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!is_alnum) { ch = '_'; }
  }
  if (value.empty()) { value = "_"; }
  if (value.front() >= '0' && value.front() <= '9') { value.insert(value.begin(), '_'); }
  return value;
}

auto symbol_name(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  if (!qualifier.has_value()) { return sanitize_symbol(name); }
  return sanitize_symbol(*qualifier) + "_" + sanitize_symbol(name);
}

auto quote_cpp_string(const std::string& value) -> std::string {
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

auto is_std_qualified(const std::string& qualifier) -> bool {
  return qualifier == "Std" || qualifier.starts_with("Std.");
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
const std::unordered_map<std::string, std::string> kBuiltinNameMap = {FLEAUX_VM_BUILTINS(FLEAUX_BUILTIN_NODE_STRING)};
#undef FLEAUX_BUILTIN_NODE_STRING

#define FLEAUX_CONST_BUILTIN_VALUE(name_literal, numeric_value) {name_literal, numeric_value},
const std::unordered_map<std::string, double> kConstantBuiltins = {
    FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_CONST_BUILTIN_VALUE)};
#undef FLEAUX_CONST_BUILTIN_VALUE

auto missing_builtin_expr(const std::string& key) -> std::string {
  return "_fleaux_missing_builtin(" + quote_cpp_string(key) + ")";
}

auto builtin_node_expr(const std::string& builtin_key) -> std::string {
  if (const auto constant_it = kConstantBuiltins.find(builtin_key); constant_it != kConstantBuiltins.end()) {
    std::ostringstream val;
    val.precision(std::numeric_limits<double>::max_digits10);
    val << constant_it->second;
    return "_fleaux_constant_builtin(" + val.str() + ")";
  }

  const auto builtin_it = kBuiltinNameMap.find(builtin_key);
  if (builtin_it == kBuiltinNameMap.end()) { return missing_builtin_expr(builtin_key); }
  return "fleaux::runtime::" + builtin_it->second + "{}";
}

auto compile_constant(const ir::IRConstant& c) -> std::string {
  if (std::get_if<std::monostate>(&c.val) != nullptr) { return "fleaux::runtime::make_null()"; }
  if (const auto* b = std::get_if<bool>(&c.val); b != nullptr) {
    return *b ? "fleaux::runtime::make_bool(true)" : "fleaux::runtime::make_bool(false)";
  }
  if (const auto* i = std::get_if<std::int64_t>(&c.val); i != nullptr) {
    return "fleaux::runtime::make_int(" + std::to_string(*i) + ")";
  }
  if (const auto* u = std::get_if<std::uint64_t>(&c.val); u != nullptr) {
    return "fleaux::runtime::make_uint(static_cast<fleaux::runtime::UInt>(" + std::to_string(*u) + "ULL))";
  }
  if (const auto* d = std::get_if<double>(&c.val); d != nullptr) {
    std::ostringstream out;
    out << "fleaux::runtime::make_float(" << *d << ")";
    return out.str();
  }
  if (const auto* s = std::get_if<std::string>(&c.val); s != nullptr) {
    return "fleaux::runtime::make_string(" + quote_cpp_string(*s) + ")";
  }
  return "fleaux::runtime::make_null()";
}

auto compile_name_ref(const ir::IRNameRef& name, const std::unordered_map<std::string, std::string>& local_bindings,
                      const std::unordered_map<std::string, std::string>& known_symbols) -> std::string {
  if (!name.qualifier.has_value()) {
    if (const auto local_it = local_bindings.find(name.name); local_it != local_bindings.end()) {
      return local_it->second;
    }

    if (const auto known_it = known_symbols.find(name.name); known_it != known_symbols.end()) {
      return known_it->second;
    }

    return sanitize_symbol(name.name);
  }

  const std::string qualified = *name.qualifier + "." + name.name;
  if (const auto known_it = known_symbols.find(qualified); known_it != known_symbols.end()) { return known_it->second; }

  if (is_std_qualified(*name.qualifier)) { return builtin_node_expr(qualified); }

  return symbol_name(name.qualifier, name.name);
}

auto compile_call_target(const ir::IRCallTarget& target,
                         const std::unordered_map<std::string, std::string>& local_bindings,
                         const std::unordered_map<std::string, std::string>& known_symbols) -> std::string {
  if (const auto* op = std::get_if<ir::IROperatorRef>(&target); op != nullptr) {
    const auto op_it = kOperatorToBuiltin.find(op->op);
    const std::string builtin = op_it == kOperatorToBuiltin.end() ? op->op : op_it->second;
    return builtin_node_expr("Std." + builtin);
  }

  const auto* name = std::get_if<ir::IRNameRef>(&target);
  if (name->qualifier.has_value() && is_std_qualified(*name->qualifier)) {
    return builtin_node_expr(*name->qualifier + "." + name->name);
  }
  return compile_name_ref(*name, local_bindings, known_symbols);
}

auto compile_expr(const ir::IRExpr& expr, const std::unordered_map<std::string, std::string>& local_bindings,
                  const std::unordered_map<std::string, std::string>& known_symbols) -> std::string {
  if (const auto* flow = std::get_if<ir::IRFlowExpr>(&expr.node); flow != nullptr) {
    return "(" + compile_expr(*flow->lhs, local_bindings, known_symbols) + " | " +
           compile_call_target(flow->rhs, local_bindings, known_symbols) + ")";
  }

  if (const auto* tuple = std::get_if<ir::IRTupleExpr>(&expr.node); tuple != nullptr) {
    if (tuple->items.size() == 1) {
      if (const auto* name_ref = std::get_if<ir::IRNameRef>(&tuple->items[0]->node);
          name_ref != nullptr && (name_ref->qualifier.has_value() || !local_bindings.contains(name_ref->name))) {
        const std::string fn_ref = compile_name_ref(*name_ref, local_bindings, known_symbols);
        return "fleaux::runtime::make_callable_ref(" + fn_ref + ")";
      }
      return compile_expr(*tuple->items[0], local_bindings, known_symbols);
    }
    std::vector<std::string> parts;
    parts.reserve(tuple->items.size());
    for (const auto& item : tuple->items) {
      if (const auto* name_ref = std::get_if<ir::IRNameRef>(&item->node);
          name_ref != nullptr && (name_ref->qualifier.has_value() || !local_bindings.contains(name_ref->name))) {
        const std::string fn_ref = compile_name_ref(*name_ref, local_bindings, known_symbols);
        parts.push_back("fleaux::runtime::make_callable_ref(" + fn_ref + ")");
        continue;
      }
      parts.push_back(compile_expr(*item, local_bindings, known_symbols));
    }

    std::ostringstream out;
    out << "fleaux::runtime::make_tuple(";
    for (std::size_t part_index = 0; part_index < parts.size(); ++part_index) {
      if (part_index > 0) { out << ", "; }
      out << parts[part_index];
    }
    out << ")";
    return out.str();
  }

  if (const auto* constant = std::get_if<ir::IRConstant>(&expr.node); constant != nullptr) {
    return compile_constant(*constant);
  }

  if (const auto* closure_ptr = std::get_if<ir::IRClosureExprBox>(&expr.node); closure_ptr != nullptr) {
    const auto& closure = **closure_ptr;
    std::ostringstream out;

    std::vector<std::string> capture_names;
    capture_names.reserve(closure.captures.size());
    for (const auto& capture : closure.captures) {
      if (const auto it = local_bindings.find(capture); it == local_bindings.end()) { continue; }
      capture_names.push_back(capture);
    }

    out << "fleaux::runtime::make_callable_ref([";
    for (std::size_t idx = 0; idx < capture_names.size(); ++idx) {
      if (idx > 0U) { out << ", "; }
      const auto& capture_name = capture_names[idx];
      out << sanitize_symbol(capture_name) << " = " << local_bindings.at(capture_name);
    }
    out << "](fleaux::runtime::Value _fleaux_arg) mutable -> fleaux::runtime::Value { ";

    std::unordered_map<std::string, std::string> closure_locals;
    for (const auto& capture_name : capture_names) { closure_locals[capture_name] = sanitize_symbol(capture_name); }
    for (const auto& p : closure.params) { closure_locals[p.name] = sanitize_symbol(p.name); }

    const bool has_variadic_tail = !closure.params.empty() && closure.params.back().type.variadic;
    if (!closure.params.empty()) {
      if (!has_variadic_tail) {
        if (closure.params.size() == 1U) {
          out << "fleaux::runtime::Value " << closure_locals[closure.params[0].name]
              << " = fleaux::runtime::unwrap_singleton_arg(_fleaux_arg); ";
        } else {
          out << "const fleaux::runtime::Value& _fleaux_args = _fleaux_arg; ";
          for (std::size_t idx = 0; idx < closure.params.size(); ++idx) {
            out << "fleaux::runtime::Value " << closure_locals[closure.params[idx].name]
                << " = fleaux::runtime::array_at(_fleaux_args, " << idx << "); ";
          }
        }
      } else if (closure.params.size() == 1U) {
        out << "fleaux::runtime::Value " << closure_locals[closure.params[0].name]
            << " = _fleaux_arg.HasArray() ? _fleaux_arg : fleaux::runtime::make_tuple(_fleaux_arg); ";
      } else {
        out << "const fleaux::runtime::Array& _fleaux_args = fleaux::runtime::as_array(_fleaux_arg); ";
        out << "if (_fleaux_args.Size() < " << (closure.params.size() - 1U)
            << ") { throw std::runtime_error(\"too few arguments for inline closure\"); } ";
        for (std::size_t idx = 0; idx + 1U < closure.params.size(); ++idx) {
          out << "fleaux::runtime::Value " << closure_locals[closure.params[idx].name] << " = *_fleaux_args.TryGet("
              << idx << "); ";
        }
        out << "fleaux::runtime::Array _fleaux_variadic_tail; ";
        out << "_fleaux_variadic_tail.Reserve(_fleaux_args.Size() - " << (closure.params.size() - 1U) << "); ";
        out << "for (std::size_t _i = " << (closure.params.size() - 1U)
            << "; _i < _fleaux_args.Size(); ++_i) { _fleaux_variadic_tail.PushBack(*_fleaux_args.TryGet(_i)); } ";
        out << "fleaux::runtime::Value " << closure_locals[closure.params.back().name]
            << " = fleaux::runtime::Value{std::move(_fleaux_variadic_tail)}; ";
      }
    }

    out << "return " << compile_expr(*closure.body, closure_locals, known_symbols) << "; })";
    return out.str();
  }

  return compile_name_ref(*std::get_if<ir::IRNameRef>(&expr.node), local_bindings, known_symbols);
}

auto emit_let_definition(const ir::IRLet& let, const std::unordered_map<std::string, std::string>& known_symbols)
    -> std::string {
  const std::string fn_name = symbol_name(let.qualifier, let.name);
  std::ostringstream out;
  out << "fleaux::runtime::Value " << fn_name << "(fleaux::runtime::Value _fleaux_arg) {\n";

  if (let.is_builtin) {
    std::string builtin_key = let.name;
    if (let.qualifier.has_value()) { builtin_key = *let.qualifier + "." + let.name; }
    out << "  return (_fleaux_arg | " << builtin_node_expr(builtin_key) << ");\n";
    out << "}\n\n";
    return out.str();
  }

  std::unordered_map<std::string, std::string> local_bindings;
  for (const auto& param : let.params) { local_bindings[param.name] = sanitize_symbol(param.name); }

  const bool has_variadic_tail = !let.params.empty() && let.params.back().type.variadic;
  if (!let.params.empty()) {
    if (!has_variadic_tail) {
      if (let.params.size() == 1U) {
        out << "  fleaux::runtime::Value " << local_bindings[let.params[0].name]
            << " = fleaux::runtime::unwrap_singleton_arg(_fleaux_arg);\n";
      } else {
        out << "  const fleaux::runtime::Value& _fleaux_args = _fleaux_arg;\n";
        for (std::size_t idx = 0; idx < let.params.size(); ++idx) {
          out << "  fleaux::runtime::Value " << local_bindings[let.params[idx].name]
              << " = fleaux::runtime::array_at(_fleaux_args, " << idx << ");\n";
        }
      }
    } else if (let.params.size() == 1U) {
      out << "  fleaux::runtime::Value " << local_bindings[let.params[0].name]
          << " = _fleaux_arg.HasArray() ? _fleaux_arg : fleaux::runtime::make_tuple(_fleaux_arg);\n";
    } else {
      out << "  const fleaux::runtime::Array& _fleaux_args = fleaux::runtime::as_array(_fleaux_arg);\n";
      out << "  if (_fleaux_args.Size() < " << (let.params.size() - 1U) << ") {\n";
      out << "    throw std::runtime_error(\"too few arguments for '" << fn_name << "'\");\n";
      out << "  }\n";
      for (std::size_t idx = 0; idx + 1U < let.params.size(); ++idx) {
        out << "  fleaux::runtime::Value " << local_bindings[let.params[idx].name] << " = *_fleaux_args.TryGet(" << idx
            << ");\n";
      }
      out << "  fleaux::runtime::Array _fleaux_variadic_tail;\n";
      out << "  _fleaux_variadic_tail.Reserve(_fleaux_args.Size() - " << (let.params.size() - 1U) << ");\n";
      out << "  for (std::size_t _i = " << (let.params.size() - 1U) << "; _i < _fleaux_args.Size(); ++_i) {\n";
      out << "    _fleaux_variadic_tail.PushBack(*_fleaux_args.TryGet(_i));\n";
      out << "  }\n";
      out << "  fleaux::runtime::Value " << local_bindings[let.params.back().name]
          << " = fleaux::runtime::Value{std::move(_fleaux_variadic_tail)};\n";
    }
  }

  out << "  return " << compile_expr(*let.body, local_bindings, known_symbols) << ";\n";
  out << "}\n\n";
  return out.str();
}

auto make_transpile_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                          const std::optional<diag::SourceSpan>& span = std::nullopt) -> TranspileError {
  return TranspileError{.message = message, .hint = hint, .span = span};
}

}  // namespace

auto FleauxCppTranspiler::process(const std::filesystem::path& source_file) const -> TranspileResult {
  auto merged_result = frontend::source_loader::load_ir_program<TranspileError>(
      source_file, make_transpile_error, "Cyclic import detected while transpiling.",
      "Break the cycle by moving shared definitions into a third module.");
  if (!merged_result) { return tl::unexpected(merged_result.error()); }

  const auto module_name = sanitize_symbol(source_file.stem().string());
  auto output = source_file.parent_path() / ("fleaux_generated_module_" + module_name + ".cpp");

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

auto FleauxCppTranspiler::emit_cpp(const ir::IRProgram& program, const std::string& module_name) const -> std::string {
  std::unordered_map<std::string, std::string> known_symbols;
  for (const auto& let : program.lets) {
    const std::string symbol = symbol_name(let.qualifier, let.name);
    if (!let.qualifier.has_value()) { known_symbols[let.name] = symbol; }
    if (let.qualifier.has_value()) { known_symbols[*let.qualifier + "." + let.name] = symbol; }
  }

  std::ostringstream out;
  out << "#include \"fleaux/runtime/runtime_support.hpp\"\n";
  out << "#include <stdexcept>\n";
  out << "#include <string>\n\n";
  out << "using fleaux::runtime::operator|;\n\n";
  out << "namespace {\n";
  out << "struct _FleauxMissingBuiltin {\n";
  out << "  const char* name;\n";
  out << "  fleaux::runtime::Value operator()(fleaux::runtime::Value) const {\n";
  out << "    throw std::runtime_error(std::string(\"Builtin '\") + name + \"' is not yet implemented in "
         "fleaux/runtime/runtime_support.hpp\");\n";
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

  for (const auto& let : program.lets) { out << emit_let_definition(let, known_symbols); }

  out << "fleaux::runtime::Value _fleaux_run_module() {\n";
  out << "  fleaux::runtime::Value _fleaux_last_value = fleaux::runtime::make_null();\n";
  for (const auto& [expr, span] : program.expressions) {
    std::unordered_map<std::string, std::string> no_locals;
    out << "  _fleaux_last_value = " << compile_expr(expr, no_locals, known_symbols) << ";\n";
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
