#pragma once
// Core builtins: sequence access, arithmetic, comparison, logical, output, control flow.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include <future>
#include <vector>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

inline constexpr std::string_view k_match_wildcard_sentinel = "__fleaux_match_wildcard__";

inline auto require_result_tuple(const Value& value, const char* op_name) -> const Array& {
    const auto& result = as_array(value);
    if (result.Size() != 2) {
        throw std::invalid_argument{std::string(op_name) + ": expected Result tuple (tag, payload)"};
    }

    if (const Value& tag = *result.TryGet(0); !tag.HasBool()) {
        throw std::invalid_argument{std::string(op_name) + ": Result tag must be a Bool (true for Ok, false for Err)"};
    }
    return result;
}

inline auto normalize_runtime_error_message(std::string message) -> std::string {
    // Unwrap nested runtime wrappers like:
    // "native 'branch_call' threw: native builtin 'Std.X' threw: actual message"
    constexpr std::string_view k_threw = "threw: ";
    while (message.starts_with("native ")) {
        const std::size_t split = message.find(k_threw);
        if (split == std::string::npos) {
            break;
        }
        message = message.substr(split + k_threw.size());
    }
    return message;
}

struct Wrap {
    // arg = any Value  ->  [arg]
    auto operator()(Value value) const -> Value {
        return make_tuple(std::move(value));
    }
};

struct Unwrap {
    // arg = [v]  ->  v
    auto operator()(Value value) const -> Value {
        return array_at(value, 0);
    }
};

struct ElementAt {
    // arg = [sequence, index]
    auto operator()(Value arg) const -> Value {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t idx = as_index_strict(array_at(arg, 1), "ElementAt index");
        auto result = seq.TryGet(idx);
        if (!result) throw std::out_of_range{"ElementAt: index out of range"};
        return *result;
    }
};

struct Length {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    auto operator()(Value arg) const -> Value {
        return make_int(static_cast<Int>(as_array(arg).Size()));
    }
};

struct Take {
    // arg = [sequence, count]
    auto operator()(Value arg) const -> Value {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t take_count = std::min(as_index_strict(array_at(arg, 1), "Take count"), seq.Size());
        Array out;
        out.Reserve(take_count);
        for (std::size_t index = 0; index < take_count; ++index) {
            out.PushBack(*seq.TryGet(index));
        }
        return Value{std::move(out)};
    }
};

struct Drop {
    // arg = [sequence, count]
    auto operator()(Value arg) const -> Value {
        const auto& seq   = as_array(array_at(arg, 0));
        const std::size_t start = as_index_strict(array_at(arg, 1), "Drop count");
        Array out;
        for (std::size_t index = start; index < seq.Size(); ++index) {
            out.PushBack(*seq.TryGet(index));
        }
        return Value{std::move(out)};
    }
};

struct Slice {
    // arg = [sequence, stop]
    //    | [sequence, start, stop]
    //    | [sequence, start, stop, step]
    auto operator()(Value arg) const -> Value {
        const auto& arr = as_array(arg);
        if (arr.Size() < 2) throw std::invalid_argument{"Slice: need at least 2 arguments"};
        const auto& seq = as_array(*arr.TryGet(0));

        std::size_t real_start{0};
        std::size_t real_stop{0};
        std::size_t real_step{1};
        if (arr.Size() == 2) {
            real_stop  = as_index_strict(*arr.TryGet(1), "Slice stop");
        } else if (arr.Size() == 3) {
            real_start = as_index_strict(*arr.TryGet(1), "Slice start");
            real_stop  = as_index_strict(*arr.TryGet(2), "Slice stop");
        } else {
            real_start = as_index_strict(*arr.TryGet(1), "Slice start");
            real_stop  = as_index_strict(*arr.TryGet(2), "Slice stop");
            real_step  = as_index_strict(*arr.TryGet(3), "Slice step");
            if (real_step == 0) throw std::invalid_argument{"Slice: step cannot be 0"};
        }

        Array out;
        const std::size_t end = std::min(real_stop, seq.Size());
        for (std::size_t index = real_start; index < end; index += real_step) {
            out.PushBack(*seq.TryGet(index));
        }
        return Value{std::move(out)};
    }
};

// Arithmetic

inline auto require_same_integer_kind(const Value& lhs, const Value& rhs, const char* op_name) -> void {
    if (is_mixed_signed_unsigned_integer_pair(lhs, rhs)) {
        throw std::invalid_argument{
            std::string(op_name) + ": cannot mix Int64 and UInt64 operands without explicit cast"};
    }
}

struct Add {
    // arg = [lhs, rhs]
    auto operator()(Value arg) const -> Value {
        const Value& lhs = array_at(arg, 0);
        const Value& rhs = array_at(arg, 1);
        // String concatenation
        if (lhs.HasString() && rhs.HasString()) {
            return make_string(as_string(lhs) + as_string(rhs));
        }
        require_same_integer_kind(lhs, rhs, "Add");
        return num_result(to_double(lhs) + to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
    }
};

struct Subtract {
    auto operator()(Value arg) const -> Value {
        const Value& lhs = array_at(arg, 0);
        const Value& rhs = array_at(arg, 1);
        require_same_integer_kind(lhs, rhs, "Subtract");
        return num_result(to_double(lhs) - to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
    }
};

struct Multiply {
    auto operator()(Value arg) const -> Value {
        const Value& lhs = array_at(arg, 0);
        const Value& rhs = array_at(arg, 1);
        require_same_integer_kind(lhs, rhs, "Multiply");
        return num_result(to_double(lhs) * to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
    }
};

struct Divide {
    auto operator()(Value arg) const -> Value {
        const Value& lhs = array_at(arg, 0);
        const Value& rhs = array_at(arg, 1);
        require_same_integer_kind(lhs, rhs, "Divide");
        return num_result(to_double(lhs) / to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
    }
};

struct Mod {
    auto operator()(Value arg) const -> Value {
        const Value& lhs = array_at(arg, 0);
        const Value& rhs = array_at(arg, 1);
        require_same_integer_kind(lhs, rhs, "Mod");
        return num_result(std::fmod(to_double(lhs), to_double(rhs)), is_uint_number(lhs) && is_uint_number(rhs));
    }
};

struct Pow {
    auto operator()(Value arg) const -> Value {
        return num_result(std::pow(to_double(array_at(arg, 0)), to_double(array_at(arg, 1))));
    }
};

struct BitAnd {
    auto operator()(Value arg) const -> Value {
        return make_int(as_int_value_strict(array_at(arg, 0), "BitAnd lhs") &
                        as_int_value_strict(array_at(arg, 1), "BitAnd rhs"));
    }
};

struct BitOr {
    auto operator()(Value arg) const -> Value {
        return make_int(as_int_value_strict(array_at(arg, 0), "BitOr lhs") |
                        as_int_value_strict(array_at(arg, 1), "BitOr rhs"));
    }
};

struct BitXor {
    auto operator()(Value arg) const -> Value {
        return make_int(as_int_value_strict(array_at(arg, 0), "BitXor lhs") ^
                        as_int_value_strict(array_at(arg, 1), "BitXor rhs"));
    }
};

struct BitNot {
    auto operator()(Value arg) const -> Value {
        return make_int(~as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "BitNot value"));
    }
};

struct BitShiftLeft {
    auto operator()(Value arg) const -> Value {
        const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftLeft value");
        const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftLeft shift");
        if (shift < 0) { throw std::invalid_argument{"BitShiftLeft: shift must be non-negative"}; }
        return make_int(value << shift);
    }
};

struct BitShiftRight {
    auto operator()(Value arg) const -> Value {
        const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftRight value");
        const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftRight shift");
        if (shift < 0) { throw std::invalid_argument{"BitShiftRight: shift must be non-negative"}; }
        return make_int(value >> shift);
    }
};

struct Sqrt {
    auto operator()(Value arg) const -> Value {
        return num_result(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct UnaryMinus {
    auto operator()(Value arg) const -> Value {
        return num_result(-to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct UnaryPlus {
    auto operator()(Value arg) const -> Value {
        return num_result(+to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct Sin {
    auto operator()(Value arg) const -> Value {
        return num_result(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Cos {
    auto operator()(Value arg) const -> Value {
        return num_result(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Tan {
    auto operator()(Value arg) const -> Value {
        return num_result(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

// Comparison & logical

struct GreaterThan {
    auto operator()(Value arg) const -> Value {
        return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) > 0);
    }
};
struct LessThan {
    auto operator()(Value arg) const -> Value {
        return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) < 0);
    }
};
struct GreaterOrEqual {
    auto operator()(Value arg) const -> Value {
        return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) >= 0);
    }
};
struct LessOrEqual {
    auto operator()(Value arg) const -> Value {
        return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) <= 0);
    }
};

struct Equal {
    auto operator()(Value arg) const -> Value {
        return make_bool(array_at(arg, 0) == array_at(arg, 1));
    }
};
struct NotEqual {
    auto operator()(Value arg) const -> Value {
        return make_bool(array_at(arg, 0) != array_at(arg, 1));
    }
};

struct Not {
    auto operator()(Value arg) const -> Value {
        return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg))));
    }
};
struct And {
    auto operator()(Value arg) const -> Value {
        return make_bool(as_bool(array_at(arg, 0)) && as_bool(array_at(arg, 1)));
    }
};
struct Or {
    auto operator()(Value arg) const -> Value {
        return make_bool(as_bool(array_at(arg, 0)) || as_bool(array_at(arg, 1)));
    }
};

// Output

struct Println {
    // Prints the value, returns it unchanged.
    auto operator()(Value arg) const -> Value {
        print_value_varargs(std::cout, arg);
        std::cout << '\n';
        return arg;
    }
};

struct Printf {
    // arg = [format, arg0, arg1, ...]
    // Prints formatted text and returns (format, (arg0, arg1, ...)).
    auto operator()(Value arg) const -> Value {
        const auto& args = as_array(arg);
        if (args.Size() < 1) {
            throw std::invalid_argument{"Printf expects at least 1 argument"};
        }
        const std::string fmt = to_string(*args.TryGet(0));
        std::vector<Value> values;
        values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);

        Array returned_args;
        returned_args.Reserve(args.Size() > 0 ? args.Size() - 1 : 0);
        for (std::size_t arg_index = 1; arg_index < args.Size(); ++arg_index) {
            const Value value = *args.TryGet(arg_index);
            values.push_back(value);
            returned_args.PushBack(value);
        }

        std::cout << format_values(fmt, values) << '\n';
        return make_tuple(make_string(fmt), Value{std::move(returned_args)});
    }
};

struct Input {
    // arg = [] | [prompt] | prompt
    auto operator()(Value arg) const -> Value {
        auto read_line = []() -> Value {
            std::string line;
            if (!std::getline(std::cin, line)) {
                return make_string("");
            }
            return make_string(line);
        };

        if (!arg.HasArray()) {
            std::cout << to_string(arg);
            std::cout.flush();
            return read_line();
        }

        const auto& args = as_array(arg);
        if (args.Size() == 0) {
            return read_line();
        }
        if (args.Size() == 1) {
            std::cout << to_string(*args.TryGet(0));
            std::cout.flush();
            return read_line();
        }
        throw std::invalid_argument{"Input expects 0 or 1 argument"};
    }
};

struct GetArgs {
    auto operator()(Value arg) const -> Value {
        (void)require_args(arg, 0, "GetArgs");
        Array out;
        const auto& args = get_process_args();
        out.Reserve(args.size());
        for (const auto& process_arg : args) {
            out.PushBack(make_string(process_arg));
        }
        return Value{std::move(out)};
    }
};

        struct Type {
            // arg = [value] | value -> String runtime type name
            auto operator()(Value arg) const -> Value {
                return make_string(type_name(unwrap_singleton_arg(std::move(arg))));
            }
        };

        struct TypeOf {
            // Alias of Type for compatibility with Std.TypeOf
            auto operator()(Value arg) const -> Value {
                return Type{}(std::move(arg));
            }
        };

struct ToInt64 {
    auto operator()(Value arg) const -> Value {
        return make_int(as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "ToInt64"));
    }
};

struct ToUInt64 {
    auto operator()(Value arg) const -> Value {
        const Value value = unwrap_singleton_arg(std::move(arg));
        return as_number(value).Visit(
            [](const Int signed_value) -> Value {
                if (signed_value < 0) {
                    throw std::invalid_argument{"ToUInt64: cannot cast negative Int64 to UInt64"};
                }
                return make_uint(static_cast<UInt>(signed_value));
            },
            [](const UInt unsigned_value) -> Value { return make_uint(unsigned_value); },
            [](const Float float_value) -> Value {
                if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0) {
                    throw std::invalid_argument{"ToUInt64: cannot cast Float64 value to UInt64"};
                }
                if (float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
                    throw std::out_of_range{"ToUInt64: Float64 value out of UInt64 range"};
                }
                return make_uint(static_cast<UInt>(float_value));
            });
    }
};

struct ToFloat64 {
    auto operator()(Value arg) const -> Value {
        return make_float(to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct Exit {
    auto operator()(Value arg) const -> Value {
        if (arg.HasArray()) {
            const auto& args = as_array(arg);
            if (args.Size() == 0) {
                std::exit(0);
            }
            if (args.Size() == 1) {
                std::exit(static_cast<int>(to_double(*args.TryGet(0))));
            }
            throw std::invalid_argument{"Exit expects 0 or 1 argument"};
        }
        std::exit(static_cast<int>(to_double(arg)));
    }
};


// Control flow (templated: functions remain concrete C++ callables)

struct Select {
    // arg = [condition, true_val, false_val] — all Values
    auto operator()(Value arg) const -> Value {
        return as_bool(array_at(arg, 0))
            ? array_at(arg, 1)
            : array_at(arg, 2);
    }
};

struct Match {
    // arg = [value, [pattern, handler], [pattern, handler], ...]
    // Pattern wildcard is encoded as the lowering sentinel string.
    // Callable patterns are predicates: pattern(subject) -> Bool.
    auto operator()(Value arg) const -> Value {
        const auto& args = as_array(arg);
        if (args.Size() < 2) {
            throw std::invalid_argument{"Match expects a value and at least one case"};
        }

        const Value subject = *args.TryGet(0);
        for (std::size_t case_index = 1; case_index < args.Size(); ++case_index) {
            const auto& case_tuple = as_array(*args.TryGet(case_index));
            if (case_tuple.Size() != 2) {
                throw std::invalid_argument{"Match case must be a (pattern, handler) tuple"};
            }

            const Value pattern = *case_tuple.TryGet(0);
            const Value handler = *case_tuple.TryGet(1);

            const bool wildcard_match = pattern.HasString() && as_string(pattern) == k_match_wildcard_sentinel;
            bool predicate_match = false;
            if (!wildcard_match && callable_id_from_value(pattern).has_value()) {
                predicate_match = as_bool(invoke_callable_ref(pattern, subject));
            }

            if (wildcard_match || predicate_match || pattern == subject) {
                return invoke_callable_ref(handler, subject);
            }
        }

        throw std::runtime_error{"Match: no case matched and no wildcard case provided"};
    }
};

struct ResultOk {
    auto operator()(Value arg) const -> Value {
        return make_tuple(make_bool(true), unwrap_singleton_arg(std::move(arg)));
    }
};

struct ResultErr {
    auto operator()(Value arg) const -> Value {
        return make_tuple(make_bool(false), unwrap_singleton_arg(std::move(arg)));
    }
};

struct ResultTag {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Tag");
        return *tuple.TryGet(0);
    }
};

struct ResultPayload {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Payload");
        return *tuple.TryGet(1);
    }
};

struct ResultIsOk {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.IsOk");
        return *tuple.TryGet(0);  // true is Ok, false is Err
    }
};

struct ResultIsErr {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.IsErr");
        return make_bool(!as_bool(*tuple.TryGet(0)));  // negation of Ok
    }
};

struct ResultUnwrap {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Unwrap");
        if (!as_bool(*tuple.TryGet(0))) {
            throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"};
        }
        return *tuple.TryGet(1);
    }
};

struct ResultUnwrapErr {
    auto operator()(Value arg) const -> Value {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.UnwrapErr");
        if (as_bool(*tuple.TryGet(0))) {
            throw std::runtime_error{"Result.UnwrapErr expected Err (false), got Ok (true)"};
        }
        return *tuple.TryGet(1);
    }
};

struct Try {
    // arg = [value, func_ref]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "Try");
        const Value& value = *args.TryGet(0);
        const Value& function_ref = *args.TryGet(1);
        try {
            return ResultOk{}(make_tuple(invoke_callable_ref(function_ref, value)));
        } catch (const std::exception& ex) {
            return ResultErr{}(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
        }
    }
};

struct ExpParallel {
    // arg = [items_tuple, func_ref]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "Exp.Parallel");
        const auto& items = as_array(*args.TryGet(0));
        const Value function_ref = *args.TryGet(1);

        std::vector<std::future<Value>> futures;
        futures.reserve(items.Size());
        for (std::size_t item_index = 0; item_index < items.Size(); ++item_index) {
            const Value item = *items.TryGet(item_index);
            futures.push_back(std::async(std::launch::async, [function_ref, item]() -> Value {
                return invoke_callable_ref(function_ref, item);
            }));
        }

        Array out;
        out.Reserve(items.Size());
        for (std::size_t future_index = 0; future_index < futures.size(); ++future_index) {
            try {
                out.PushBack(futures[future_index].get());
            } catch (const std::exception& ex) {
                return ResultErr{}(make_tuple(
                    make_tuple(make_int(static_cast<Int>(future_index)),
                               make_string(normalize_runtime_error_message(ex.what())))));
            }
        }
        return ResultOk{}(make_tuple(Value{std::move(out)}));
    }
};

struct Apply {
    // arg = [value, func_ref]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "Apply");
        return invoke_callable_ref(*args.TryGet(1), *args.TryGet(0));
    }
};

struct Branch {
    // arg = [condition, value, true_func_ref, false_func_ref]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 4, "Branch");
        const Value& condition = *args.TryGet(0);
        const Value& value = *args.TryGet(1);
        const Value& true_func = *args.TryGet(2);
        const Value& false_func = *args.TryGet(3);
        return as_bool(condition) ? invoke_callable_ref(true_func, value) : invoke_callable_ref(false_func, value);
    }
};

struct Loop {
    // arg = [state, continue_func_ref, step_func_ref]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 3, "Loop");
        Value state = *args.TryGet(0);
        const Value& continue_func = *args.TryGet(1);
        const Value& step_func = *args.TryGet(2);
        while (as_bool(invoke_callable_ref(continue_func, state))) {
            state = invoke_callable_ref(step_func, std::move(state));
        }
        return state;
    }
};

struct LoopN {
    // arg = [state, continue_func_ref, step_func_ref, max_iters]
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 4, "LoopN");
        Value state = *args.TryGet(0);
        const Value& continue_func = *args.TryGet(1);
        const Value& step_func = *args.TryGet(2);
        const std::size_t max_iters = as_index_strict(*args.TryGet(3), "LoopN max_iters");

        std::size_t steps = 0;
        while (as_bool(invoke_callable_ref(continue_func, state))) {
            if (steps >= max_iters) {
                throw std::runtime_error{"LoopN: exceeded max_iters"};
            }
            state = invoke_callable_ref(step_func, std::move(state));
            ++steps;
        }
        return state;
    }
};

}  // namespace fleaux::runtime
