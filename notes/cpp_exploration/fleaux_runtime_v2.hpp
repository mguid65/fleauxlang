// fleaux_runtime_v2.hpp - C++20 runtime support for Fleaux - strongly typed version
#ifndef FLEAUX_RUNTIME_V2_HPP
#define FLEAUX_RUNTIME_V2_HPP

#include <iostream>
#include <string>
#include <tuple>
#include <functional>
#include <cmath>
#include <utility>
#include <variant>

namespace fleaux {

// Strongly typed tuple wrapper with pipe operator support
template <typename T>
class Value {
public:
    Value() = default;
    explicit Value(T val) : value_(val) {}

    T get() const { return value_; }
    operator T() const { return value_; }

private:
    T value_;
};

// Pipe operator: (value) | function
template <typename In, typename Out>
Value<Out> operator|(const Value<In>& lhs, std::function<Value<Out>(const Value<In>&)> rhs) {
    return rhs(lhs);
}

// Pipe tuple into function: tuple<Ts...> | std::function<R(std::tuple<Ts...>)>
template <typename... Ts, typename Out>
Out operator|(const std::tuple<Ts...>& lhs, std::function<Out(std::tuple<Ts...>)> rhs) {
    return rhs(lhs);
}

// Pipe tuple into function that takes unpacked arguments via std::apply
template <typename... Ts, typename R, typename Func>
R apply_pipe(const std::tuple<Ts...>& lhs, Func&& func) {
    return std::apply(func, lhs);
}

// Generic pipe for std::function - attempts to call function with tuple directly
template <typename In, typename Out>
Out operator|(const In& lhs, std::function<Out(In)> rhs) {
    return rhs(lhs);
}

// Arithmetic operators for tuples of two numbers
inline double operator_add(const std::tuple<double, double>& args) {
    return std::get<0>(args) + std::get<1>(args);
}

inline double operator_subtract(const std::tuple<double, double>& args) {
    return std::get<0>(args) - std::get<1>(args);
}

inline double operator_multiply(const std::tuple<double, double>& args) {
    return std::get<0>(args) * std::get<1>(args);
}

inline double operator_divide(const std::tuple<double, double>& args) {
    auto [a, b] = args;
    if (b == 0.0) throw std::runtime_error("Division by zero");
    return a / b;
}

inline double operator_mod(const std::tuple<double, double>& args) {
    return std::fmod(std::get<0>(args), std::get<1>(args));
}

inline double operator_pow(const std::tuple<double, double>& args) {
    return std::pow(std::get<0>(args), std::get<1>(args));
}

// Comparison operators for tuples of two numbers
inline bool operator_equal(const std::tuple<double, double>& args) {
    return std::get<0>(args) == std::get<1>(args);
}

inline bool operator_not_equal(const std::tuple<double, double>& args) {
    return std::get<0>(args) != std::get<1>(args);
}

inline bool operator_less_than(const std::tuple<double, double>& args) {
    return std::get<0>(args) < std::get<1>(args);
}

inline bool operator_greater_than(const std::tuple<double, double>& args) {
    return std::get<0>(args) > std::get<1>(args);
}

inline bool operator_less_or_equal(const std::tuple<double, double>& args) {
    return std::get<0>(args) <= std::get<1>(args);
}

inline bool operator_greater_or_equal(const std::tuple<double, double>& args) {
    return std::get<0>(args) >= std::get<1>(args);
}

// Logical operators
inline bool operator_and(const std::tuple<bool, bool>& args) {
    return std::get<0>(args) && std::get<1>(args);
}

inline bool operator_or(const std::tuple<bool, bool>& args) {
    return std::get<0>(args) || std::get<1>(args);
}

inline bool operator_not(bool b) {
    return !b;
}

// Math functions
inline double sqrt_op(double x) {
    return std::sqrt(x);
}

inline double sin_op(double x) {
    return std::sin(x);
}

inline double cos_op(double x) {
    return std::cos(x);
}

inline double tan_op(double x) {
    return std::tan(x);
}

// Generic tuple access - ElementAt with variadic tuple support
// Handles: (tuple<T0, T1, T2, ...>, index) -> Ti
namespace detail {
    template <typename Tuple, size_t... Is>
    auto element_at_impl(const Tuple& tup, size_t idx, std::index_sequence<Is...>) {
        using result_type = std::variant<std::tuple_element_t<Is, Tuple>...>;

        result_type result;
        size_t current = 0;

        // Runtime indexing with compile-time tuple unpacking
        (..., (current == idx ? (result = std::get<Is>(tup), true) : (++current, false)));

        return result;
    }
}

// For 2-element tuples: (tuple, index)
template <typename T0, typename T1>
T0 element_at(const std::tuple<std::tuple<T0, T1>, double>& args) {
    auto [tup, idx_d] = args;
    int idx = static_cast<int>(idx_d);
    if (idx == 0) return std::get<0>(tup);
    if (idx == 1) return std::get<1>(tup);
    throw std::runtime_error("ElementAt: index out of bounds");
}

// For 3-element tuples: (tuple, index)
template <typename T0, typename T1, typename T2>
std::variant<T0, T1, T2> element_at(const std::tuple<std::tuple<T0, T1, T2>, double>& args) {
    auto [tup, idx_d] = args;
    int idx = static_cast<int>(idx_d);
    if (idx == 0) return std::get<0>(tup);
    if (idx == 1) return std::get<1>(tup);
    if (idx == 2) return std::get<2>(tup);
    throw std::runtime_error("ElementAt: index out of bounds");
}

// For 4-element tuples
template <typename T0, typename T1, typename T2, typename T3>
std::variant<T0, T1, T2, T3> element_at(const std::tuple<std::tuple<T0, T1, T2, T3>, double>& args) {
    auto [tup, idx_d] = args;
    int idx = static_cast<int>(idx_d);
    if (idx == 0) return std::get<0>(tup);
    if (idx == 1) return std::get<1>(tup);
    if (idx == 2) return std::get<2>(tup);
    if (idx == 3) return std::get<3>(tup);
    throw std::runtime_error("ElementAt: index out of bounds");
}

// Wrap single value in 1-tuple
template <typename T>
std::tuple<T> wrap_value(const T& value) {
    return std::make_tuple(value);
}

// Unwrap single value from 1-tuple
template <typename T>
T unwrap_value(const std::tuple<T>& tup) {
    return std::get<0>(tup);
}

// Println for any type
template <typename T>
T println(const T& value) {
    if constexpr (std::is_same_v<T, double>) {
        std::cout << value << std::endl;
    } else if constexpr (std::is_same_v<T, std::string>) {
        std::cout << value << std::endl;
    } else if constexpr (std::is_same_v<T, bool>) {
        std::cout << (value ? "true" : "false") << std::endl;
    }
    return value;
}

} // namespace fleaux

#endif // FLEAUX_RUNTIME_V2_HPP

