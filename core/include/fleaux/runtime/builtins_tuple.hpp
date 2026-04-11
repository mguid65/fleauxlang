#pragma once
// Tuple collection builtins (Map, Filter, Sort, Reduce, Range, Zip, etc.).
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {
// ── Tuple builtins ────────────────────────────────────────────────────────────

struct TupleAppend {
    // arg = [sequence, item]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleAppend");
        const auto& src = as_array(*args.TryGet(0));
        Array out;
        out.Reserve(src.Size() + 1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(*src.TryGet(i));
        }
        out.PushBack(*args.TryGet(1));
        return Value{std::move(out)};
    }
};

struct TuplePrepend {
    // arg = [sequence, item]  →  [item, ...sequence]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TuplePrepend");
        const auto& src = as_array(*args.TryGet(0));
        Array out;
        out.Reserve(src.Size() + 1);
        out.PushBack(*args.TryGet(1));
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(*src.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct TupleReverse {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        const auto& src = as_array(arg);
        Array out;
        out.Reserve(src.Size());
        for (std::size_t i = src.Size(); i > 0; --i) {
            out.PushBack(*src.TryGet(i - 1));
        }
        return Value{std::move(out)};
    }
};

struct TupleContains {
    // arg = [sequence, item]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleContains");
        const auto& src = as_array(*args.TryGet(0));
        const Value& item = *args.TryGet(1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (*src.TryGet(i) == item) {
                return make_bool(true);
            }
        }
        return make_bool(false);
    }
};

struct TupleZip {
    // arg = [sequenceA, sequenceB]  →  [(a0,b0), (a1,b1), ...]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleZip");
        const auto& a = as_array(*args.TryGet(0));
        const auto& b = as_array(*args.TryGet(1));
        const std::size_t n = std::min(a.Size(), b.Size());
        Array out;
        out.Reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.PushBack(make_tuple(*a.TryGet(i), *b.TryGet(i)));
        }
        return Value{std::move(out)};
    }
};

struct TupleMap {
    // arg = [sequence, func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleMap");
        const auto& src = as_array(*args.TryGet(0));
        const Value& func = *args.TryGet(1);

        Array out;
        out.Reserve(src.Size());
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(invoke_callable_ref(func, *src.TryGet(i)));
        }
        return Value{std::move(out)};
    }
};

struct TupleFilter {
    // arg = [sequence, pred_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleFilter");
        const auto& src = as_array(*args.TryGet(0));
        const Value& pred = *args.TryGet(1);

        Array out;
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (const Value& item = *src.TryGet(i); as_bool(invoke_callable_ref(pred, item))) {
                out.PushBack(item);
            }
        }
        return Value{std::move(out)};
    }
};

struct TupleSort {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        const auto& src = as_array(arg);
        std::vector<Value> items;
        items.reserve(src.Size());
        for (std::size_t i = 0; i < src.Size(); ++i) {
            items.push_back(*src.TryGet(i));
        }

        std::ranges::stable_sort(items, [](const Value& lhs, const Value& rhs) {
            return compare_values_for_sort(lhs, rhs) < 0;
        });

        Array out;
        out.Reserve(items.size());
        for (const auto& item : items) {
            out.PushBack(item);
        }
        return Value{std::move(out)};
    }
};

struct TupleUnique {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        const auto& src = as_array(arg);
        Array out;
        out.Reserve(src.Size());
        for (std::size_t i = 0; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            bool seen = false;
            for (std::size_t j = 0; j < out.Size(); ++j) {
                if (*out.TryGet(j) == item) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                out.PushBack(item);
            }
        }
        return Value{std::move(out)};
    }
};

struct TupleMin {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        const auto& src = as_array(arg);
        if (src.Size() == 0) {
            throw std::invalid_argument{"TupleMin expects non-empty tuple"};
        }
        const Value& first = *src.TryGet(0);
        const Value* best = &first;
        for (std::size_t i = 1; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            if (compare_values_for_sort(item, *best) < 0) {
                best = &item;
            }
        }
        return *best;
    }
};

struct TupleMax {
    // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
    Value operator()(Value arg) const {
        const auto& src = as_array(arg);
        if (src.Size() == 0) {
            throw std::invalid_argument{"TupleMax expects non-empty tuple"};
        }
        const Value& first = *src.TryGet(0);
        const Value* best = &first;
        for (std::size_t i = 1; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            if (compare_values_for_sort(item, *best) > 0) {
                best = &item;
            }
        }
        return *best;
    }
};

struct TupleReduce {
    // arg = [sequence, initial, func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 3, "TupleReduce");
        const auto& src = as_array(*args.TryGet(0));
        Value acc = *args.TryGet(1);
        const Value& fn = *args.TryGet(2);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            acc = invoke_callable_ref(fn, make_tuple(std::move(acc), *src.TryGet(i)));
        }
        return acc;
    }
};

struct TupleFindIndex {
    // arg = [sequence, pred_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleFindIndex");
        const auto& src = as_array(*args.TryGet(0));
        const Value& pred = *args.TryGet(1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (as_bool(invoke_callable_ref(pred, *src.TryGet(i)))) {
                return make_int(static_cast<Int>(i));
            }
        }
        return make_int(-1);
    }
};

struct TupleAny {
    // arg = [sequence, pred_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleAny");
        const auto& src = as_array(*args.TryGet(0));
        const Value& pred = *args.TryGet(1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (as_bool(invoke_callable_ref(pred, *src.TryGet(i)))) {
                return make_bool(true);
            }
        }
        return make_bool(false);
    }
};

struct TupleAll {
    // arg = [sequence, pred_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleAll");
        const auto& src = as_array(*args.TryGet(0));
        const Value& pred = *args.TryGet(1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (!as_bool(invoke_callable_ref(pred, *src.TryGet(i)))) {
                return make_bool(false);
            }
        }
        return make_bool(true);
    }
};

struct TupleRange {
    // arg = [stop] | [start, stop] | [start, stop, step]
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        Int start = 0;
        Int stop = 0;
        Int step = 1;

        if (args.Size() == 1) {
            stop = as_int_value(*args.TryGet(0));
        } else if (args.Size() == 2) {
            start = as_int_value(*args.TryGet(0));
            stop = as_int_value(*args.TryGet(1));
        } else if (args.Size() == 3) {
            start = as_int_value(*args.TryGet(0));
            stop = as_int_value(*args.TryGet(1));
            step = as_int_value(*args.TryGet(2));
        } else {
            throw std::invalid_argument{"TupleRange expects 1, 2, or 3 arguments"};
        }

        if (step == 0) {
            throw std::invalid_argument{"TupleRange step cannot be 0"};
        }

        Array out;
        if (step > 0) {
            for (Int i = start; i < stop; i += step) {
                out.PushBack(make_int(i));
            }
        } else {
            for (Int i = start; i > stop; i += step) {
                out.PushBack(make_int(i));
            }
        }
        return Value{std::move(out)};
    }
};

}  // namespace fleaux::runtime
