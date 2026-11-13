#pragma once

#include <array>
#include <span>
#include <string_view>

namespace fleaux::frontend {

// An operator stands for a named function. It calls the Std builtin of that name when the builtin accepts the
// operands; otherwise, for each operand whose type is a strong type `Name`, it tries the user function `Name.<name>`
// (for example `Money.Add` for `+`).

// Qualifier shared by every builtin an operator stands for.
inline constexpr std::string_view kOperatorBuiltinQualifier = "Std";

// A Std builtin that an operator stands for.
struct OperatorBuiltin {
  std::string_view name;  // Unqualified builtin name inside Std, e.g. "Add".
};

// Returns the Std builtins an operator can stand for. Most operators have exactly one. '-' has a unary form
// (Std.UnaryMinus) and a binary form (Std.Subtract); the type checker picks one from the argument count. Returns an
// empty span for an unknown operator.
[[nodiscard]] inline auto operator_builtins(const std::string_view op) -> std::span<const OperatorBuiltin> {
  static constexpr std::array<OperatorBuiltin, 1> kAdd{{{"Add"}}};
  static constexpr std::array<OperatorBuiltin, 2> kMinus{{{"UnaryMinus"}, {"Subtract"}}};
  static constexpr std::array<OperatorBuiltin, 1> kMultiply{{{"Multiply"}}};
  static constexpr std::array<OperatorBuiltin, 1> kDivide{{{"Divide"}}};
  static constexpr std::array<OperatorBuiltin, 1> kMod{{{"Mod"}}};
  static constexpr std::array<OperatorBuiltin, 1> kPow{{{"Pow"}}};
  static constexpr std::array<OperatorBuiltin, 1> kEqual{{{"Equal"}}};
  static constexpr std::array<OperatorBuiltin, 1> kNotEqual{{{"NotEqual"}}};
  static constexpr std::array<OperatorBuiltin, 1> kLessThan{{{"LessThan"}}};
  static constexpr std::array<OperatorBuiltin, 1> kGreaterThan{{{"GreaterThan"}}};
  static constexpr std::array<OperatorBuiltin, 1> kLessOrEqual{{{"LessOrEqual"}}};
  static constexpr std::array<OperatorBuiltin, 1> kGreaterOrEqual{{{"GreaterOrEqual"}}};
  static constexpr std::array<OperatorBuiltin, 1> kNot{{{"Not"}}};
  static constexpr std::array<OperatorBuiltin, 1> kAnd{{{"And"}}};
  static constexpr std::array<OperatorBuiltin, 1> kOr{{{"Or"}}};

  if (op == "+") {
    return kAdd;
  }
  if (op == "-") {
    return kMinus;
  }
  if (op == "*") {
    return kMultiply;
  }
  if (op == "/") {
    return kDivide;
  }
  if (op == "%") {
    return kMod;
  }
  if (op == "^") {
    return kPow;
  }
  if (op == "==") {
    return kEqual;
  }
  if (op == "!=") {
    return kNotEqual;
  }
  if (op == "<") {
    return kLessThan;
  }
  if (op == ">") {
    return kGreaterThan;
  }
  if (op == "<=") {
    return kLessOrEqual;
  }
  if (op == ">=") {
    return kGreaterOrEqual;
  }
  if (op == "!") {
    return kNot;
  }
  if (op == "&&") {
    return kAnd;
  }
  if (op == "||") {
    return kOr;
  }
  return {};
}

// The operator symbols, so their function names can be listed.
inline constexpr std::array<std::string_view, 15> kOperatorSymbols{"+",  "-", "*",  "/",  "%",  "^",  "==", "!=",
                                                                   "<",  ">", "<=", ">=", "!",  "&&", "||"};

// The operator a function name stands for when it is declared on a strong type (`Money.Add` is `+`), if any.
[[nodiscard]] inline auto operator_for_function_name(const std::string_view name) -> std::string_view {
  for (const auto symbol : kOperatorSymbols) {
    for (const auto& builtin : operator_builtins(symbol)) {
      if (builtin.name == name) {
        return symbol;
      }
    }
  }
  return {};
}

}  // namespace fleaux::frontend
