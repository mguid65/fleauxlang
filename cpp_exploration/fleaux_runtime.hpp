// fleaux_runtime.hpp - C++20 runtime support for Fleaux dataflow language
#ifndef FLEAUX_RUNTIME_HPP
#define FLEAUX_RUNTIME_HPP

#include <any>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace fleaux {

// Flexible value type for runtime tuple operations
using FlexValue = std::any;

// Function type for dataflow nodes
using FlowFn = std::function<FlexValue(const FlexValue&)>;

// Dataflow node wrapper
class FlowNode {
public:
    explicit FlowNode(FlowFn fn = nullptr) : fn_(fn) {}

    FlexValue operator()(const FlexValue& input) const {
        if (!fn_) {
            throw std::runtime_error("FlowNode is not initialized");
        }
        return fn_(input);
    }

    // Pipe operator: left | right
    friend FlexValue operator|(const FlexValue& lhs, const FlowNode& rhs) {
        return rhs(lhs);
    }

private:
    FlowFn fn_;
};

// Create a builtin node from a function name
inline FlowNode make_builtin_node(const std::string& name);

// Forward declarations for builtins
namespace builtins {

inline FlexValue ElementAt(const FlexValue& input) {
    std::cerr << "ElementAt called with input type: " << input.type().name() << std::endl;

    // The input might be:
    // 1. A FlexValue containing std::tuple<FlexValue, FlexValue> - the (tuple, index) args
    // 2. Or directly a std::tuple<FlexValue, FlexValue> if we're being called with an unwrapped tuple

    std::tuple<FlexValue, FlexValue> args;

    // Try first interpretation - input IS a FlexValue containing a tuple
    try {
        args = std::any_cast<std::tuple<FlexValue, FlexValue>>(input);
        std::cerr << "Input is FlexValue containing tuple<FlexValue,FlexValue>" << std::endl;
    } catch (const std::bad_any_cast&) {
        // Try second interpretation - input itself is the tuple
        try {
            auto tup = std::any_cast<std::tuple<FlexValue, FlexValue>>(input);
            args = tup;
            std::cerr << "Input is directly tuple<FlexValue,FlexValue>" << std::endl;
        } catch (const std::bad_any_cast&) {
            throw std::runtime_error("ElementAt: input must be (tuple, index) args");
        }
    }

    auto seq_flex = std::get<0>(args);
    auto idx_value = std::get<1>(args);

    std::cerr << "seq_flex type: " << seq_flex.type().name() << std::endl;
    std::cerr << "idx_value type: " << idx_value.type().name() << std::endl;

    try {
        int idx = static_cast<int>(std::any_cast<double>(idx_value));
        std::cerr << "idx = " << idx << std::endl;

        // Now seq_flex is a FlexValue. It might contain:
        // A) A 3-tuple directly
        // B) A tuple wrapped in another any
        try {
            auto seq = std::any_cast<std::tuple<FlexValue, FlexValue, FlexValue>>(seq_flex);
            std::cerr << "Unpacked seq as 3-tuple of FlexValues" << std::endl;
            if (idx == 0) return std::get<0>(seq);
            if (idx == 1) return std::get<1>(seq);
            if (idx == 2) return std::get<2>(seq);
            throw std::runtime_error("ElementAt: index out of bounds for 3-tuple");
        } catch (const std::bad_any_cast&) {
            std::cerr << "Not a 3-tuple of FlexValues" << std::endl;
            throw std::runtime_error("ElementAt: sequence is not a 3-tuple");
        }
    } catch (const std::bad_any_cast&) {
        throw std::runtime_error("ElementAt: index is not a number");
    }
}

inline FlexValue Wrap(const FlexValue& input) {
    return FlexValue(std::make_tuple(input));
}

inline FlexValue Unwrap(const FlexValue& input) {
    try {
        auto tup = std::any_cast<std::tuple<FlexValue>>(input);
        return std::get<0>(tup);
    } catch (const std::bad_any_cast& e) {
        throw std::runtime_error(std::string("Unwrap: type error - ") + e.what());
    }
}

inline FlexValue Println(const FlexValue& input) {
    try {
        double val = std::any_cast<double>(input);
        std::cout << val << std::endl;
        return FlexValue(val);
    } catch (const std::bad_any_cast&) {
        try {
            std::string val = std::any_cast<std::string>(input);
            std::cout << val << std::endl;
            return FlexValue(val);
        } catch (const std::bad_any_cast&) {
            std::cout << "Value" << std::endl;
            return input;
        }
    }
}

} // namespace builtins

// Create a builtin node from a function name
inline FlowNode make_builtin_node(const std::string& name) {
    if (name == "ElementAt") {
        return FlowNode([](const FlexValue& input) -> FlexValue {
            try {
                return builtins::ElementAt(input);
            } catch (const std::exception& e) {
                throw std::runtime_error(e.what());
            } catch (...) {
                throw std::runtime_error("ElementAt: unknown error");
            }
        });
    }
    if (name == "Wrap") return FlowNode(builtins::Wrap);
    if (name == "Unwrap") return FlowNode(builtins::Unwrap);
    if (name == "Println") return FlowNode(builtins::Println);

    return FlowNode([name](const FlexValue& input) -> FlexValue {
        throw std::runtime_error("Builtin '" + name + "' is not implemented yet");
    });
}

// Tuple packing helpers
template <typename T>
inline FlexValue make_flex(const T& value) {
    return FlexValue(value);
}

template <typename T>
inline T flex_cast(const FlexValue& value) {
    return std::any_cast<T>(value);
}

// Tuple creation
template <typename... Args>
inline FlexValue make_tuple_flex(Args&&... args) {
    return FlexValue(std::make_tuple(std::forward<Args>(args)...));
}

// Numeric operators
namespace ops {

inline FlexValue Add(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a + b);
}

inline FlexValue Subtract(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a - b);
}

inline FlexValue Multiply(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a * b);
}

inline FlexValue Divide(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    if (b == 0.0) {
        throw std::runtime_error("Division by zero");
    }
    return FlexValue(a / b);
}

inline FlexValue Mod(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(std::fmod(a, b));
}

inline FlexValue Pow(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(std::pow(a, b));
}

// Comparison operators
inline FlexValue Equal(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a == b);
}

inline FlexValue NotEqual(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a != b);
}

inline FlexValue LessThan(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a < b);
}

inline FlexValue GreaterThan(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a > b);
}

inline FlexValue LessOrEqual(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a <= b);
}

inline FlexValue GreaterOrEqual(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<double, double>>(input);
    return FlexValue(a >= b);
}

// Logical operators
inline FlexValue And(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<bool, bool>>(input);
    return FlexValue(a && b);
}

inline FlexValue Or(const FlexValue& input) {
    auto [a, b] = std::any_cast<std::tuple<bool, bool>>(input);
    return FlexValue(a || b);
}

inline FlexValue Not(const FlexValue& input) {
    auto a = std::any_cast<bool>(input);
    return FlexValue(!a);
}

// Math functions
inline FlexValue Sqrt(const FlexValue& input) {
    auto a = std::any_cast<double>(input);
    return FlexValue(std::sqrt(a));
}

inline FlexValue Sin(const FlexValue& input) {
    auto a = std::any_cast<double>(input);
    return FlexValue(std::sin(a));
}

inline FlexValue Cos(const FlexValue& input) {
    auto a = std::any_cast<double>(input);
    return FlexValue(std::cos(a));
}

inline FlexValue Tan(const FlexValue& input) {
    auto a = std::any_cast<double>(input);
    return FlexValue(std::tan(a));
}

} // namespace ops

// Create flow nodes from operator functions
inline FlowNode Add = FlowNode(ops::Add);
inline FlowNode Subtract = FlowNode(ops::Subtract);
inline FlowNode Multiply = FlowNode(ops::Multiply);
inline FlowNode Divide = FlowNode(ops::Divide);
inline FlowNode Mod = FlowNode(ops::Mod);
inline FlowNode Pow = FlowNode(ops::Pow);
inline FlowNode Equal = FlowNode(ops::Equal);
inline FlowNode NotEqual = FlowNode(ops::NotEqual);
inline FlowNode LessThan = FlowNode(ops::LessThan);
inline FlowNode GreaterThan = FlowNode(ops::GreaterThan);
inline FlowNode LessOrEqual = FlowNode(ops::LessOrEqual);
inline FlowNode GreaterOrEqual = FlowNode(ops::GreaterOrEqual);
inline FlowNode And = FlowNode(ops::And);
inline FlowNode Or = FlowNode(ops::Or);
inline FlowNode Not = FlowNode(ops::Not);
inline FlowNode Sqrt = FlowNode(ops::Sqrt);
inline FlowNode Sin = FlowNode(ops::Sin);
inline FlowNode Cos = FlowNode(ops::Cos);
inline FlowNode Tan = FlowNode(ops::Tan);

} // namespace fleaux

#endif // FLEAUX_RUNTIME_HPP

