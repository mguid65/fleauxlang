#pragma once
// Core builtins: sequence access, arithmetic, comparison, logical, output, control flow.
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {

inline constexpr std::string_view k_match_wildcard_sentinel = "__fleaux_match_wildcard__";

inline const Array& require_result_tuple(const Value& value, const char* op_name) {
    const auto& result = as_array(value);
    if (result.Size() != 2) {
        throw std::invalid_argument{std::string(op_name) + ": expected Result tuple (tag, payload)"};
    }

    const Value& tag = *result.TryGet(0);
    if (!tag.HasBool()) {
        throw std::invalid_argument{std::string(op_name) + ": Result tag must be a Bool (true for Ok, false for Err)"};
    }
    return result;
}

struct Wrap {
    // arg = any Value  →  [arg]
    Value operator()(Value v) const {
        return make_tuple(std::move(v));
    }
};

struct Unwrap {
    // arg = [v]  →  v
    Value operator()(Value v) const {
        return array_at(v, 0);
    }
};

struct ElementAt {
    // arg = [sequence, index]
    Value operator()(Value arg) const {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t idx = as_index(array_at(arg, 1));
        auto r = seq.TryGet(idx);
        if (!r) throw std::out_of_range{"ElementAt: index out of range"};
        return *r;
    }
};

struct Length {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        return make_int(static_cast<Int>(as_array(arg).Size()));
    }
};

struct Take {
    // arg = [sequence, count]
    Value operator()(Value arg) const {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t n = std::min(as_index(array_at(arg, 1)), seq.Size());
        Array out;
        out.Reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct Drop {
    // arg = [sequence, count]
    Value operator()(Value arg) const {
        const auto& seq   = as_array(array_at(arg, 0));
        const std::size_t start = as_index(array_at(arg, 1));
        Array out;
        for (std::size_t i = start; i < seq.Size(); ++i) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct Slice {
    // arg = [sequence, stop]
    //     | [sequence, start, stop]
    //     | [sequence, start, stop, step]
    Value operator()(Value arg) const {
        const auto& arr = as_array(arg);
        if (arr.Size() < 2) throw std::invalid_argument{"Slice: need at least 2 arguments"};
        const auto& seq = as_array(*arr.TryGet(0));

        std::size_t real_start{0};
        std::size_t real_stop{0};
        std::size_t real_step{1};
        if (arr.Size() == 2) {
            real_stop  = as_index(*arr.TryGet(1));
        } else if (arr.Size() == 3) {
            real_start = as_index(*arr.TryGet(1));
            real_stop  = as_index(*arr.TryGet(2));
        } else {
            real_start = as_index(*arr.TryGet(1));
            real_stop  = as_index(*arr.TryGet(2));
            real_step  = as_index(*arr.TryGet(3));
            if (real_step == 0) throw std::invalid_argument{"Slice: step cannot be 0"};
        }

        Array out;
        const std::size_t end = std::min(real_stop, seq.Size());
        for (std::size_t i = real_start; i < end; i += real_step) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

// ── Arithmetic ────────────────────────────────────────────────────────────────

struct Add {
    // arg = [lhs, rhs]
    Value operator()(Value arg) const {
        const Value& l = array_at(arg, 0);
        const Value& r = array_at(arg, 1);
        // String concatenation
        if (l.HasString() && r.HasString()) {
            return make_string(as_string(l) + as_string(r));
        }
        return num_result(to_double(l) + to_double(r));
    }
};

struct Subtract {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) - to_double(array_at(arg, 1)));
    }
};

struct Multiply {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) * to_double(array_at(arg, 1)));
    }
};

struct Divide {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) / to_double(array_at(arg, 1)));
    }
};

struct Mod {
    Value operator()(Value arg) const {
        const double l = to_double(array_at(arg, 0));
        const double r = to_double(array_at(arg, 1));
        return num_result(std::fmod(l, r));
    }
};

struct Pow {
    Value operator()(Value arg) const {
        return num_result(std::pow(to_double(array_at(arg, 0)), to_double(array_at(arg, 1))));
    }
};

struct Sqrt {
    Value operator()(Value arg) const {
        return num_result(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct UnaryMinus {
    Value operator()(Value arg) const {
        return num_result(-to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct UnaryPlus {
    Value operator()(Value arg) const {
        return num_result(+to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct Sin {
    Value operator()(Value arg) const {
        return num_result(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Cos {
    Value operator()(Value arg) const {
        return num_result(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Tan {
    Value operator()(Value arg) const {
        return num_result(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

// ── Comparison & logical ──────────────────────────────────────────────────────

struct GreaterThan {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) > to_double(array_at(arg, 1)));
    }
};
struct LessThan {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) < to_double(array_at(arg, 1)));
    }
};
struct GreaterOrEqual {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) >= to_double(array_at(arg, 1)));
    }
};
struct LessOrEqual {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) <= to_double(array_at(arg, 1)));
    }
};

struct Equal {
    Value operator()(Value arg) const {
        return make_bool(array_at(arg, 0) == array_at(arg, 1));
    }
};
struct NotEqual {
    Value operator()(Value arg) const {
        return make_bool(array_at(arg, 0) != array_at(arg, 1));
    }
};

struct Not {
    Value operator()(Value arg) const {
        return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg))));
    }
};
struct And {
    Value operator()(Value arg) const {
        return make_bool(as_bool(array_at(arg, 0)) && as_bool(array_at(arg, 1)));
    }
};
struct Or {
    Value operator()(Value arg) const {
        return make_bool(as_bool(array_at(arg, 0)) || as_bool(array_at(arg, 1)));
    }
};

// ── Output ────────────────────────────────────────────────────────────────────

struct Println {
    // Prints the value, returns it unchanged.
    Value operator()(Value arg) const {
        print_value_varargs(std::cout, arg);
        std::cout << '\n';
        return arg;
    }
};

struct Printf {
    // arg = [format, arg0, arg1, ...]
    // Prints formatted text and returns (format, (arg0, arg1, ...)).
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() < 1) {
            throw std::invalid_argument{"Printf expects at least 1 argument"};
        }
        const std::string fmt = to_string(*args.TryGet(0));
        std::vector<Value> values;
        values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);

        Array returned_args;
        returned_args.Reserve(args.Size() > 0 ? args.Size() - 1 : 0);
        for (std::size_t i = 1; i < args.Size(); ++i) {
            const Value v = *args.TryGet(i);
            values.push_back(v);
            returned_args.PushBack(v);
        }

        std::cout << format_values(fmt, values) << '\n';
        return make_tuple(make_string(fmt), Value{std::move(returned_args)});
    }
};

struct Input {
    // arg = [] | [prompt] | prompt
    Value operator()(Value arg) const {
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
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "GetArgs");
        Array out;
        const auto& args = get_process_args();
        out.Reserve(args.size());
        for (const auto& s : args) {
            out.PushBack(make_string(s));
        }
        return Value{std::move(out)};
    }
};

struct Exit {
    Value operator()(Value arg) const {
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


// ── Control flow (templated: functions remain concrete C++ callables) ─────────

struct Select {
    // arg = [condition, true_val, false_val] — all Values
    Value operator()(Value arg) const {
        return as_bool(array_at(arg, 0))
            ? array_at(arg, 1)
            : array_at(arg, 2);
    }
};

struct Match {
    // arg = [value, [pattern, handler], [pattern, handler], ...]
    // Pattern wildcard is encoded as the lowering sentinel string.
    // Callable patterns are predicates: pattern(subject) -> Bool.
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() < 2) {
            throw std::invalid_argument{"Match expects a value and at least one case"};
        }

        const Value subject = *args.TryGet(0);
        for (std::size_t i = 1; i < args.Size(); ++i) {
            const auto& case_tuple = as_array(*args.TryGet(i));
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
    Value operator()(Value arg) const {
        return make_tuple(make_bool(true), unwrap_singleton_arg(std::move(arg)));
    }
};

struct ResultErr {
    Value operator()(Value arg) const {
        return make_tuple(make_bool(false), unwrap_singleton_arg(std::move(arg)));
    }
};

struct ResultTag {
    Value operator()(Value arg) const {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Tag");
        return *tuple.TryGet(0);
    }
};

struct ResultPayload {
    Value operator()(Value arg) const {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Payload");
        return *tuple.TryGet(1);
    }
};

struct ResultIsOk {
    Value operator()(Value arg) const {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.IsOk");
        return *tuple.TryGet(0);  // true is Ok, false is Err
    }
};

struct ResultIsErr {
    Value operator()(Value arg) const {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.IsErr");
        return make_bool(!as_bool(*tuple.TryGet(0)));  // negation of Ok
    }
};

struct ResultUnwrap {
    Value operator()(Value arg) const {
        const Value result = unwrap_singleton_arg(std::move(arg));
        const auto& tuple = require_result_tuple(result, "Result.Unwrap");
        if (!as_bool(*tuple.TryGet(0))) {
            throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"};
        }
        return *tuple.TryGet(1);
    }
};

struct ResultUnwrapErr {
    Value operator()(Value arg) const {
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
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "Try");
        const Value& val = *args.TryGet(0);
        const Value& func = *args.TryGet(1);
        try {
            return ResultOk{}(make_tuple(invoke_callable_ref(func, val)));
        } catch (const std::exception& ex) {
            std::string message = ex.what();
            const std::string prefix = "native builtin '";
            const std::string separator = "' threw: ";
            if (message.rfind(prefix, 0) == 0) {
                const std::size_t split = message.find(separator);
                if (split != std::string::npos) {
                    message = message.substr(split + separator.size());
                }
            }
            return ResultErr{}(make_tuple(make_string(std::move(message))));
        }
    }
};

struct Apply {
    // arg = [value, func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "Apply");
        return invoke_callable_ref(*args.TryGet(1), *args.TryGet(0));
    }
};

struct Branch {
    // arg = [condition, value, true_func_ref, false_func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 4, "Branch");
        const Value& cond = *args.TryGet(0);
        const Value& val = *args.TryGet(1);
        const Value& tf = *args.TryGet(2);
        const Value& ff = *args.TryGet(3);
        return as_bool(cond) ? invoke_callable_ref(tf, val) : invoke_callable_ref(ff, val);
    }
};

struct Loop {
    // arg = [state, continue_func_ref, step_func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 3, "Loop");
        Value state = *args.TryGet(0);
        const Value& cf = *args.TryGet(1);
        const Value& sf = *args.TryGet(2);
        while (as_bool(invoke_callable_ref(cf, state))) {
            state = invoke_callable_ref(sf, std::move(state));
        }
        return state;
    }
};

struct LoopN {
    // arg = [state, continue_func_ref, step_func_ref, max_iters]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 4, "LoopN");
        Value state = *args.TryGet(0);
        const Value& cf = *args.TryGet(1);
        const Value& sf = *args.TryGet(2);
        const std::size_t max_iters = as_index(*args.TryGet(3));

        std::size_t steps = 0;
        while (as_bool(invoke_callable_ref(cf, state))) {
            if (steps >= max_iters) {
                throw std::runtime_error{"LoopN: exceeded max_iters"};
            }
            state = invoke_callable_ref(sf, std::move(state));
            ++steps;
        }
        return state;
    }
};

}  // namespace fleaux::runtime
