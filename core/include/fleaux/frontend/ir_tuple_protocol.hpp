#pragma once

#include <tuple>
#include <utility>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::ir {
namespace detail {

template <std::size_t I, typename T>
constexpr decltype(auto) get_ir_flow_expr(T&& value) {
  static_assert(I < 3);
  if constexpr (I == 0) {
    return (std::forward<T>(value).lhs);
  } else if constexpr (I == 1) {
    return (std::forward<T>(value).rhs);
  } else {
    return (std::forward<T>(value).span);
  }
}

template <std::size_t I, typename T>
constexpr decltype(auto) get_ir_name_ref(T&& value) {
  static_assert(I < 6);
  if constexpr (I == 0) {
    return (std::forward<T>(value).qualifier);
  } else if constexpr (I == 1) {
    return (std::forward<T>(value).name);
  } else if constexpr (I == 2) {
    return (std::forward<T>(value).explicit_type_args);
  } else if constexpr (I == 3) {
    return (std::forward<T>(value).resolved_symbol_key);
  } else if constexpr (I == 4) {
    return (std::forward<T>(value).materialize_as_value);
  } else {
    return (std::forward<T>(value).span);
  }
}

template <std::size_t I, typename T>
constexpr decltype(auto) get_ir_block_expr_statement(T&& value) {
  static_assert(I < 2);
  if constexpr (I == 0) {
    return (std::forward<T>(value).expr);
  } else {
    return (std::forward<T>(value).span);
  }
}

template <std::size_t I, typename T>
constexpr decltype(auto) get_ir_local_let(T&& value) {
  static_assert(I < 4);
  if constexpr (I == 0) {
    return (std::forward<T>(value).name);
  } else if constexpr (I == 1) {
    return (std::forward<T>(value).type);
  } else if constexpr (I == 2) {
    return (std::forward<T>(value).expr);
  } else {
    return (std::forward<T>(value).span);
  }
}

}  // namespace detail

template <std::size_t I>
constexpr decltype(auto) get(IRFlowExpr& value) {
  return detail::get_ir_flow_expr<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(const IRFlowExpr& value) {
  return detail::get_ir_flow_expr<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(IRFlowExpr&& value) {
  return detail::get_ir_flow_expr<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(const IRFlowExpr&& value) {
  return detail::get_ir_flow_expr<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(IRNameRef& value) {
  return detail::get_ir_name_ref<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(const IRNameRef& value) {
  return detail::get_ir_name_ref<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(IRNameRef&& value) {
  return detail::get_ir_name_ref<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(const IRNameRef&& value) {
  return detail::get_ir_name_ref<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(IRBlockExprStatement& value) {
  return detail::get_ir_block_expr_statement<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(const IRBlockExprStatement& value) {
  return detail::get_ir_block_expr_statement<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(IRBlockExprStatement&& value) {
  return detail::get_ir_block_expr_statement<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(const IRBlockExprStatement&& value) {
  return detail::get_ir_block_expr_statement<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(IRLocalLet& value) {
  return detail::get_ir_local_let<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(const IRLocalLet& value) {
  return detail::get_ir_local_let<I>(value);
}

template <std::size_t I>
constexpr decltype(auto) get(IRLocalLet&& value) {
  return detail::get_ir_local_let<I>(std::move(value));
}

template <std::size_t I>
constexpr decltype(auto) get(const IRLocalLet&& value) {
  return detail::get_ir_local_let<I>(std::move(value));
}

}  // namespace fleaux::frontend::ir

namespace std {

template <>
struct tuple_size<fleaux::frontend::ir::IRFlowExpr> : integral_constant<std::size_t, 3> {};

template <>
struct tuple_element<0, fleaux::frontend::ir::IRFlowExpr> {
  using type = fleaux::frontend::ir::IRExprBox;
};

template <>
struct tuple_element<1, fleaux::frontend::ir::IRFlowExpr> {
  using type = fleaux::frontend::ir::IRCallTarget;
};

template <>
struct tuple_element<2, fleaux::frontend::ir::IRFlowExpr> {
  using type = std::optional<fleaux::frontend::diag::SourceSpan>;
};

template <>
struct tuple_size<fleaux::frontend::ir::IRNameRef> : integral_constant<std::size_t, 6> {};

template <>
struct tuple_element<0, fleaux::frontend::ir::IRNameRef> {
  using type = std::optional<std::string>;
};

template <>
struct tuple_element<1, fleaux::frontend::ir::IRNameRef> {
  using type = std::string;
};

template <>
struct tuple_element<2, fleaux::frontend::ir::IRNameRef> {
  using type = std::vector<fleaux::frontend::ir::IRSimpleType>;
};

template <>
struct tuple_element<3, fleaux::frontend::ir::IRNameRef> {
  using type = std::optional<std::string>;
};

template <>
struct tuple_element<4, fleaux::frontend::ir::IRNameRef> {
  using type = bool;
};

template <>
struct tuple_element<5, fleaux::frontend::ir::IRNameRef> {
  using type = std::optional<fleaux::frontend::diag::SourceSpan>;
};

template <>
struct tuple_size<fleaux::frontend::ir::IRBlockExprStatement> : integral_constant<std::size_t, 2> {};

template <>
struct tuple_element<0, fleaux::frontend::ir::IRBlockExprStatement> {
  using type = fleaux::frontend::ir::IRExprBox;
};

template <>
struct tuple_element<1, fleaux::frontend::ir::IRBlockExprStatement> {
  using type = std::optional<fleaux::frontend::diag::SourceSpan>;
};

template <>
struct tuple_size<fleaux::frontend::ir::IRLocalLet> : integral_constant<std::size_t, 4> {};

template <>
struct tuple_element<0, fleaux::frontend::ir::IRLocalLet> {
  using type = std::string;
};

template <>
struct tuple_element<1, fleaux::frontend::ir::IRLocalLet> {
  using type = fleaux::frontend::ir::IRSimpleType;
};

template <>
struct tuple_element<2, fleaux::frontend::ir::IRLocalLet> {
  using type = fleaux::frontend::ir::IRExprBox;
};

template <>
struct tuple_element<3, fleaux::frontend::ir::IRLocalLet> {
  using type = std::optional<fleaux::frontend::diag::SourceSpan>;
};

}  // namespace std

