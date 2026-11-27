// The text of Lowerer::dump_ir, which `fleaux --dump-ir` prints: each IR node as a block of its fields, nested by
// indentation.

#include "fleaux/frontend/lowering.hpp"

#include <format>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/utility.hpp"
#include "fleaux/frontend/ir_tuple_protocol.hpp"

namespace fleaux::frontend::lowering {
namespace {

class IRDumper {
public:
  [[nodiscard]] auto dump(const ir::IRProgram& program) const -> std::string { return format_program(program, 0); }

private:
  [[nodiscard]] static auto indent(const int level) -> std::string {
    const std::size_t count = static_cast<std::size_t>(level) * 2U;
    // Clang-Tidy is wrong here. If you use braced-init here then the wrong overload is selected.
    // NOLINTNEXTLINE(modernize-return-braced-init-list)
    return std::string(count, ' ');
  }

  [[nodiscard]] static auto escape_string(const std::string_view text) -> std::string {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
      switch (ch) {
        case '\\':
          escaped += "\\\\";
          break;
        case '\"':
          escaped += "\\\"";
          break;
        case '\n':
          escaped += "\\n";
          break;
        case '\r':
          escaped += "\\r";
          break;
        case '\t':
          escaped += "\\t";
          break;
        default:
          escaped.push_back(ch);
          break;
      }
    }
    return escaped;
  }

  [[nodiscard]] static auto quote(const std::string_view text) -> std::string { return std::format("\"{}\"", escape_string(text)); }

  [[nodiscard]] static auto format_list(const std::vector<std::string>& items, const int level) -> std::string {
    if (items.empty()) {
      return "[]";
    }

    std::string out = "[\n";
    for (std::size_t idx = 0; idx < items.size(); ++idx) {
      out += indent(level + 1) + items[idx];
      if (idx + 1 < items.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + ']';
    return out;
  }

  [[nodiscard]] static auto format_string_list(const std::vector<std::string>& items, const int level) -> std::string {
    std::vector<std::string> formatted;
    formatted.reserve(items.size());
    for (const auto& item : items) {
      formatted.push_back(quote(item));
    }
    return format_list(formatted, level);
  }

  template <typename Range, typename Formatter>
  [[nodiscard]] static auto collect_formatted_items(const Range& items, Formatter&& formatter) -> std::vector<std::string> {
    std::vector<std::string> formatted;
    formatted.reserve(items.size());
    for (const auto& item : items) {
      formatted.push_back(std::invoke(formatter, item));
    }
    return formatted;
  }

  [[nodiscard]] static auto format_block(const std::string_view name, const std::vector<std::string>& fields, const int level)
      -> std::string {
    if (fields.empty()) {
      return std::format("{} {{}}", name);
    }

    std::string out = std::format("{} {{\n", name);
    for (std::size_t idx = 0; idx < fields.size(); ++idx) {
      out += indent(level + 1) + fields[idx];
      if (idx + 1 < fields.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + '}';
    return out;
  }

  [[nodiscard]] static auto format_qualifier(const std::optional<std::string>& qualifier) -> std::string {
    return qualifier ? quote(*qualifier) : std::string{"null"};
  }

  [[nodiscard]] auto format_nullable_expr(const ir::IRExprBox& expr, const int level) const -> std::string {
    return expr ? format_expr(*expr, level) : std::string{"null"};
  }

  [[nodiscard]] auto format_program(const ir::IRProgram& program, const int level) const -> std::string {
    const auto imports =
        collect_formatted_items(program.imports, [&](const auto& import_stmt) { return format_import(import_stmt, level + 2); });
    const auto type_decls =
        collect_formatted_items(program.type_decls, [&](const auto& type_decl) { return format_type_decl(type_decl, level + 2); });
    const auto lets = collect_formatted_items(program.lets, [&](const auto& let) { return format_let(let, level + 2); });
    const auto expressions =
        collect_formatted_items(program.expressions, [&](const auto& expr_stmt) { return format_expr_statement(expr_stmt, level + 2); });
    const auto alias_decls =
        collect_formatted_items(program.alias_decls, [&](const auto& alias_decl) { return format_alias_decl(alias_decl, level + 2); });

    return format_block(
        "IRProgram",
        {std::format("imports: {}", format_list(imports, level + 1)), std::format("type_decls: {}", format_list(type_decls, level + 1)),
         std::format("lets: {}", format_list(lets, level + 1)), std::format("expressions: {}", format_list(expressions, level + 1)),
         std::format("alias_decls: {}", format_list(alias_decls, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_import(const ir::IRImport& import_stmt, const int level) -> std::string {
    return format_block("IRImport", {std::format("module_name: {}", quote(import_stmt.module_name))}, level);
  }

  [[nodiscard]] static auto format_type_decl(const ir::IRTypeDecl& type_decl, const int level) -> std::string {
    return format_block(
        "IRTypeDecl",
        {std::format("name: {}", quote(type_decl.name)),
         std::format("generic_params: {}", format_string_list(type_decl.generic_params, level + 1)),
         std::format("is_builtin: {}", type_decl.is_builtin),
         std::format("target: {}", type_decl.is_builtin ? std::string{"null"} : format_simple_type(type_decl.target, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_alias_decl(const ir::IRAliasDecl& alias_decl, const int level) -> std::string {
    return format_block(
        "IRAliasDecl",
        {std::format("name: {}", quote(alias_decl.name)), std::format("target: {}", format_simple_type(alias_decl.target, level + 1))},
        level);
  }

  [[nodiscard]] auto format_let(const ir::IRLet& let, const int level) const -> std::string {
    const auto params = collect_formatted_items(let.params, [&](const auto& param) { return format_param(param, level + 2); });

    return format_block(
        "IRLet",
        {std::format("qualifier: {}", format_qualifier(let.qualifier)), std::format("name: {}", quote(let.name)),
         std::format("symbol_key: {}", quote(let.symbol_key)),
         std::format("generic_params: {}", format_string_list(let.generic_params, level + 1)),
         std::format("params: {}", format_list(params, level + 1)),
         std::format("return_type: {}", format_simple_type(let.return_type, level + 1)),
         std::format("doc_comments: {}", format_string_list(let.doc_comments, level + 1)),
         std::format("body: {}", format_nullable_expr(let.body, level + 1)), std::format("is_value_binding: {}", let.is_value_binding),
         std::format("is_builtin: {}", let.is_builtin), std::format("is_native: {}", let.is_native)},
        level);
  }

  [[nodiscard]] auto format_block_expr_statement(const ir::IRBlockExprStatement& stmt, const int level) const -> std::string {
    return format_block("IRBlockExprStatement", {std::format("expr: {}", format_nullable_expr(stmt.expr, level + 1))}, level);
  }

  [[nodiscard]] auto format_local_let(const ir::IRLocalLet& let, const int level) const -> std::string {
    return format_block("IRLocalLet",
                        {std::format("name: {}", quote(let.name)), std::format("type: {}", format_simple_type(let.type, level + 1)),
                         std::format("expr: {}", format_nullable_expr(let.expr, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_block_item(const ir::IRBlockItem& item, const int level) const -> std::string {
    return utility::structured_visit<std::string>(
        common::overloaded{[&](const std::string& name, const ir::IRSimpleType& type, const ir::IRExprBox& expr,
                               const std::optional<diag::SourceSpan>&) -> std::string {
                             return format_local_let(ir::IRLocalLet{.name = name, .type = type, .expr = expr}, level);
                           },
                           [&](const ir::IRExprBox& expr, const std::optional<diag::SourceSpan>&) -> std::string {
                             return format_block_expr_statement(ir::IRBlockExprStatement{.expr = expr}, level);
                           }},
        item);
  }

  [[nodiscard]] auto format_block_expr(const ir::IRBlockExpr& block, const int level) const -> std::string {
    const auto items = collect_formatted_items(block.items, [&](const auto& item) { return format_block_item(item, level + 2); });
    return format_block(
        "IRBlockExpr",
        {std::format("items: {}", format_list(items, level + 1)), std::format("result: {}", format_nullable_expr(block.result, level + 1))},
        level);
  }

  [[nodiscard]] auto format_expr_statement(const ir::IRExprStatement& stmt, const int level) const -> std::string {
    return format_block("IRExprStatement", {std::format("expr: {}", format_expr(stmt.expr, level + 1))}, level);
  }

  [[nodiscard]] static auto format_param(const ir::IRParam& param, const int level) -> std::string {
    return format_block(
        "IRParam", {std::format("name: {}", quote(param.name)), std::format("type: {}", format_simple_type(param.type, level + 1))}, level);
  }

  [[nodiscard]] static auto format_function_signature(const ir::IRSimpleType::FunctionSignature& signature, const int level)
      -> std::string {
    const auto function_params =
        collect_formatted_items(signature.param_types, [&](const auto& param_type) { return format_simple_type(param_type, level + 2); });
    return format_block("FunctionSignature",
                        {std::format("param_types: {}", format_list(function_params, level + 1)),
                         std::format("return_type: {}",
                                     signature.return_type ? format_simple_type(*signature.return_type, level + 1) : std::string{"null"})},
                        level);
  }

  [[nodiscard]] static auto format_simple_type(const ir::IRSimpleType& type, const int level) -> std::string {
    const auto alternative_types =
        collect_formatted_items(type.alternative_types, [&](const auto& alt) { return format_simple_type(alt, level + 2); });
    const auto tuple_items =
        collect_formatted_items(type.tuple_items, [&](const auto& item) { return format_simple_type(item, level + 2); });
    const auto type_args = collect_formatted_items(type.type_args, [&](const auto& arg) { return format_simple_type(arg, level + 2); });

    std::vector fields{
        std::format("name: {}", quote(type.name)),
        std::format("variadic: {}", type.variadic),
        std::format("alternative_types: {}", format_list(alternative_types, level + 1)),
        std::format("tuple_items: {}", format_list(tuple_items, level + 1)),
        std::format("type_args: {}", format_list(type_args, level + 1)),
    };

    if (type.function_sig.has_value()) {
      fields.push_back(std::format("function_sig: {}", format_function_signature(*type.function_sig, level + 1)));
    } else {
      fields.emplace_back("function_sig: null");
    }

    return format_block("IRSimpleType", fields, level);
  }

  [[nodiscard]] auto format_expr(const ir::IRExpr& expr, const int level) const -> std::string {
    return format_block(
        "IRExpr",
        {std::format("kind: {}", quote(expr_kind_name(expr.node))), std::format("node: {}", format_expr_node(expr.node, level + 1))},
        level);
  }

  [[nodiscard]] static auto expr_kind_name(
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox, ir::IRBlockExprBox>& node)
      -> std::string_view {
    return std::visit(common::overloaded{[](const ir::IRFlowExpr&) -> std::string_view { return "IRFlowExpr"; },
                                         [](const ir::IRTupleExpr&) -> std::string_view { return "IRTupleExpr"; },
                                         [](const ir::IRConstant&) -> std::string_view { return "IRConstant"; },
                                         [](const ir::IRNameRef&) -> std::string_view { return "IRNameRef"; },
                                         [](const ir::IRClosureExprBox&) -> std::string_view { return "IRClosureExpr"; },
                                         [](const ir::IRBlockExprBox&) -> std::string_view { return "IRBlockExpr"; }},
                      node);
  }

  [[nodiscard]] auto format_expr_node(
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox, ir::IRBlockExprBox>& node,
      const int level) const -> std::string {
    return std::visit(common::overloaded{[&](const ir::IRFlowExpr& flow) -> std::string { return format_flow_expr(flow, level); },
                                         [&](const ir::IRTupleExpr& tuple) -> std::string { return format_tuple_expr(tuple, level); },
                                         [&](const ir::IRConstant& constant) -> std::string { return format_constant(constant, level); },
                                         [&](const ir::IRNameRef& name_ref) -> std::string { return format_name_ref(name_ref, level); },
                                         [&](const ir::IRClosureExprBox& closure) -> std::string {
                                           return closure ? format_closure_expr(*closure, level) : std::string{"null"};
                                         },
                                         [&](const ir::IRBlockExprBox& block) -> std::string {
                                           return block ? format_block_expr(*block, level) : std::string{"null"};
                                         }},
                      node);
  }

  [[nodiscard]] auto format_flow_expr(const ir::IRFlowExpr& flow, const int level) const -> std::string {
    return format_block("IRFlowExpr",
                        {std::format("lhs: {}", format_nullable_expr(flow.lhs, level + 1)),
                         std::format("rhs: {}", format_call_target(flow.rhs, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_tuple_expr(const ir::IRTupleExpr& tuple, const int level) const -> std::string {
    const auto items = collect_formatted_items(tuple.items, [&](const auto& item) { return format_nullable_expr(item, level + 2); });
    return format_block("IRTupleExpr", {std::format("items: {}", format_list(items, level + 1))}, level);
  }

  [[nodiscard]] auto format_closure_expr(const ir::IRClosureExpr& closure, const int level) const -> std::string {
    const auto params = collect_formatted_items(closure.params, [&](const auto& param) { return format_param(param, level + 2); });

    return format_block("IRClosureExpr",
                        {std::format("generic_params: {}", format_string_list(closure.generic_params, level + 1)),
                         std::format("params: {}", format_list(params, level + 1)),
                         std::format("return_type: {}", format_simple_type(closure.return_type, level + 1)),
                         std::format("body: {}", format_nullable_expr(closure.body, level + 1)),
                         std::format("captures: {}", format_string_list(closure.captures, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_call_target(const ir::IRCallTarget& target, const int level) -> std::string {
    return std::visit(
        common::overloaded{[&](const ir::IRNameRef& name_ref) -> std::string { return format_name_ref(name_ref, level); },
                           [&](const ir::IROperatorRef& op_ref) -> std::string { return format_operator_ref(op_ref, level); }},
        target);
  }

  [[nodiscard]] static auto format_name_ref(const ir::IRNameRef& name_ref, const int level) -> std::string {
    const auto type_args =
        collect_formatted_items(name_ref.explicit_type_args, [&](const auto& type_arg) { return format_simple_type(type_arg, level + 2); });

    return format_block(
        "IRNameRef",
        {std::format("qualifier: {}", format_qualifier(name_ref.qualifier)), std::format("name: {}", quote(name_ref.name)),
         std::format("explicit_type_args: {}", format_list(type_args, level + 1)),
         std::format("resolved_symbol_key: {}", name_ref.resolved_symbol_key ? quote(*name_ref.resolved_symbol_key) : std::string{"null"}),
         std::format("materialize_as_value: {}", name_ref.materialize_as_value)},
        level);
  }

  [[nodiscard]] static auto format_operator_ref(const ir::IROperatorRef& op_ref, const int level) -> std::string {
    return format_block("IROperatorRef", {std::format("op: {}", quote(op_ref.op))}, level);
  }

  [[nodiscard]] static auto format_constant(const ir::IRConstant& constant, const int level) -> std::string {
    const auto format_constant_value = [](const auto& value) -> std::string {
      return common::overloaded{[](const std::int64_t int_value) -> std::string { return std::format("{}", int_value); },
                                [](const std::uint64_t uint_value) -> std::string { return std::format("{}u64", uint_value); },
                                [](const double double_value) -> std::string { return std::format("{}", double_value); },
                                [](const bool bool_value) -> std::string { return bool_value ? "true" : "false"; },
                                [](const std::string& string_value) -> std::string { return quote(string_value); },
                                [](const std::monostate&) -> std::string { return std::string{"null"}; }}(value);
    };

    return format_block("IRConstant", {std::format("value: {}", std::visit(format_constant_value, constant.val))}, level);
  }
};

}  // namespace

auto Lowerer::dump_ir(const ir::IRProgram& program) const -> std::string { return IRDumper{}.dump(program); }

}  // namespace fleaux::frontend::lowering
