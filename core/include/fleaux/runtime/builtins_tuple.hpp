#pragma once
// Tuple collection builtins (Map, Filter, Sort, Reduce, Range, Zip, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {
// Tuple builtins

// arg = [sequence, item]
[[nodiscard]] inline auto TupleAppend(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleAppend");
  const auto& src = as_array(*args.TryGet(0));
  Array out;
  out.Reserve(src.Size() + 1);
  for (std::size_t index = 0; index < src.Size(); ++index) { out.PushBack(*src.TryGet(index)); }
  out.PushBack(*args.TryGet(1));
  return Value{std::move(out)};
}

// arg = [sequence, item]  ->  [item, ...sequence]
[[nodiscard]] inline auto TuplePrepend(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TuplePrepend");
  const auto& src = as_array(*args.TryGet(0));
  Array out;
  out.Reserve(src.Size() + 1);
  out.PushBack(*args.TryGet(1));
  for (std::size_t index = 0; index < src.Size(); ++index) { out.PushBack(*src.TryGet(index)); }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleReverse(Value arg) -> Value {
  const auto& src = as_array(arg);
  Array out;
  out.Reserve(src.Size());
  for (std::size_t reverse_index = src.Size(); reverse_index > 0; --reverse_index) {
    out.PushBack(*src.TryGet(reverse_index - 1));
  }
  return Value{std::move(out)};
}

// arg = [sequence, item]
[[nodiscard]] inline auto TupleContains(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleContains");
  const auto& src = as_array(*args.TryGet(0));
  const Value& item = *args.TryGet(1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (*src.TryGet(index) == item) { return make_bool(true); }
  }
  return make_bool(false);
}

// arg = [sequenceA, sequenceB]  ->  [(a0,b0), (a1,b1), ...]
[[nodiscard]] inline auto TupleZip(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleZip");
  const auto& lhs_arr = as_array(*args.TryGet(0));
  const auto& rhs_arr = as_array(*args.TryGet(1));
  const std::size_t min_size = std::min(lhs_arr.Size(), rhs_arr.Size());
  Array out;
  out.Reserve(min_size);
  for (std::size_t index = 0; index < min_size; ++index) {
    out.PushBack(make_tuple(*lhs_arr.TryGet(index), *rhs_arr.TryGet(index)));
  }
  return Value{std::move(out)};
}

// arg = [sequence, func_ref]
[[nodiscard]] inline auto TupleMap(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleMap");
  const auto& src = as_array(*args.TryGet(0));
  const Value& function_ref = *args.TryGet(1);

  Array out;
  out.Reserve(src.Size());
  for (std::size_t index = 0; index < src.Size(); ++index) {
    out.PushBack(invoke_callable_ref(function_ref, *src.TryGet(index)));
  }
  return Value{std::move(out)};
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFilter(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleFilter");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);

  Array out;
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (const Value& item = *src.TryGet(index); as_bool(invoke_callable_ref(pred, item))) { out.PushBack(item); }
  }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleSort(Value arg) -> Value {
  const auto& src = as_array(arg);
  std::vector<Value> items;
  items.reserve(src.Size());
  for (std::size_t index = 0; index < src.Size(); ++index) { items.push_back(*src.TryGet(index)); }

  std::ranges::stable_sort(items, [](const Value& lhs, const Value& rhs) -> bool {
    return compare_values_for_sort(lhs, rhs) < 0;
  });

  Array out;
  out.Reserve(items.size());
  for (const auto& item : items) { out.PushBack(item); }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleUnique(Value arg) -> Value {
  const auto& src = as_array(arg);
  Array out;
  out.Reserve(src.Size());
  for (std::size_t index = 0; index < src.Size(); ++index) {
    const Value& item = *src.TryGet(index);
    bool seen = false;
    for (std::size_t existing_index = 0; existing_index < out.Size(); ++existing_index) {
      if (*out.TryGet(existing_index) == item) {
        seen = true;
        break;
      }
    }
    if (!seen) { out.PushBack(item); }
  }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleMin(Value arg) -> Value {
  const auto& src = as_array(arg);
  if (src.Size() == 0) { throw std::invalid_argument{"TupleMin expects non-empty tuple"}; }
  const Value& first = *src.TryGet(0);
  const Value* best = &first;
  for (std::size_t index = 1; index < src.Size(); ++index) {
    const Value& item = *src.TryGet(index);
    if (compare_values_for_sort(item, *best) < 0) { best = &item; }
  }
  return *best;
}

// arg = sequence
[[nodiscard]] inline auto TupleMax(Value arg) -> Value {
  const auto& src = as_array(arg);
  if (src.Size() == 0) { throw std::invalid_argument{"TupleMax expects non-empty tuple"}; }
  const Value& first = *src.TryGet(0);
  const Value* best = &first;
  for (std::size_t index = 1; index < src.Size(); ++index) {
    const Value& item = *src.TryGet(index);
    if (compare_values_for_sort(item, *best) > 0) { best = &item; }
  }
  return *best;
}

// arg = [sequence, initial, func_ref]
[[nodiscard]] inline auto TupleReduce(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "TupleReduce");
  const auto& src = as_array(*args.TryGet(0));
  Value accumulator = *args.TryGet(1);
  const Value& reducer = *args.TryGet(2);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    accumulator = invoke_callable_ref(reducer, make_tuple(std::move(accumulator), *src.TryGet(index)));
  }
  return accumulator;
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFindIndex(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleFindIndex");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (as_bool(invoke_callable_ref(pred, *src.TryGet(index)))) { return make_int(static_cast<Int>(index)); }
  }
  return make_int(-1);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAny(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleAny");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (as_bool(invoke_callable_ref(pred, *src.TryGet(index)))) { return make_bool(true); }
  }
  return make_bool(false);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAll(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleAll");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (!as_bool(invoke_callable_ref(pred, *src.TryGet(index)))) { return make_bool(false); }
  }
  return make_bool(true);
}

// arg = [stop] | [start, stop] | [start, stop, step]
[[nodiscard]] inline auto TupleRange(Value arg) -> Value {
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

  if (step == 0) { throw std::invalid_argument{"TupleRange step cannot be 0"}; }

  Array out;
  if (step > 0) {
    for (Int current = start; current < stop; current += step) { out.PushBack(make_int(current)); }
  } else {
    for (Int current = start; current > stop; current += step) { out.PushBack(make_int(current)); }
  }
  return Value{std::move(out)};
}

}  // namespace fleaux::runtime
