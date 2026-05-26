#pragma once
// Tuple collection builtins (Map, Filter, Sort, Reduce, Range, Zip, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

template <typename Visitor>
void for_each_tuple_value(const Array& src, Visitor&& visitor) {
  for (std::size_t index = 0; index < src.Size(); ++index) {
    visitor(*src.TryGet(index));
  }
}

template <typename Predicate>
[[nodiscard]] auto tuple_find_index_if(const Array& src, Predicate&& predicate) -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (const Value& item = *src.TryGet(index); predicate(item)) {
      return index;
    }
  }
  return std::nullopt;
}

template <typename Predicate>
[[nodiscard]] auto tuple_any_of(const Array& src, Predicate&& predicate) -> bool {
  return tuple_find_index_if(src, predicate).has_value();
}

template <typename Predicate>
[[nodiscard]] auto tuple_all_of(const Array& src, Predicate&& predicate) -> bool {
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (const Value& item = *src.TryGet(index); !predicate(item)) {
      return false;
    }
  }
  return true;
}

// Tuple builtins

// arg = [sequence, item]
[[nodiscard]] inline auto TupleAppend(Value arg) -> Value {
  const auto& src = as_array(array_at(arg, 0));
  Array out;
  out.Reserve(src.Size() + 1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    out.PushBack(*src.TryGet(index));
  }
  out.PushBack(array_at(arg, 1));
  return Value{std::move(out)};
}

// arg = [sequence, item]  ->  [item, ...sequence]
[[nodiscard]] inline auto TuplePrepend(Value arg) -> Value {
  const auto& src = as_array(array_at(arg, 0));
  Array out;
  out.Reserve(src.Size() + 1);
  out.PushBack(array_at(arg, 1));
  for (std::size_t index = 0; index < src.Size(); ++index) {
    out.PushBack(*src.TryGet(index));
  }
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
  const auto& src = as_array(array_at(arg, 0));
  const Value& item = array_at(arg, 1);
  return make_bool(tuple_any_of(src, [&](const Value& candidate) -> bool { return candidate == item; }));
}

// arg = [sequenceA, sequenceB]  ->  [(a0,b0), (a1,b1), ...]
[[nodiscard]] inline auto TupleZip(Value arg) -> Value {
  const auto& lhs_arr = as_array(array_at(arg, 0));
  const auto& rhs_arr = as_array(array_at(arg, 1));
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
  auto& src = as_array(array_at(arg, 0));
  const Value& function_ref = array_at(arg, 1);
  const RuntimeCallable function = resolve_callable_ref(function_ref);

  Array out;
  out.Reserve(src.Size());
  for (std::size_t index = 0; index < src.Size(); ++index) {
    out.PushBack(function(std::move(src[index])));
  }
  return Value{std::move(out)};
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFilter(Value arg) -> Value {
  auto& src = as_array(array_at(arg, 0));
  const Value& pred = array_at(arg, 1);
  const RuntimeCallable predicate = resolve_callable_ref(pred);

  Array out;
  out.Reserve(src.Size());
  for (std::size_t index = 0; index < src.Size(); ++index) {
    Value item = std::move(src[index]);
    if (as_bool(predicate(item))) {
      out.PushBack(std::move(item));
    }
  }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleSort(Value arg) -> Value {
  const auto& src = as_array(arg);
  std::vector<Value> items;
  items.reserve(src.Size());
  for_each_tuple_value(src, [&](const Value& item) { items.push_back(item); });

  std::ranges::stable_sort(
      items, [](const Value& lhs, const Value& rhs) -> bool { return compare_values_for_sort(lhs, rhs) < 0; });

  Array out;
  out.Reserve(items.size());
  for (const auto& item : items) {
    out.PushBack(item);
  }
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
    if (!seen) {
      out.PushBack(item);
    }
  }
  return Value{std::move(out)};
}

// arg = sequence
[[nodiscard]] inline auto TupleMin(Value arg) -> Value {
  const auto& src = as_array(arg);
  if (src.Size() == 0) {
    throw std::invalid_argument{"TupleMin expects non-empty tuple"};
  }
  const Value& first = *src.TryGet(0);
  const Value* best = &first;
  for (std::size_t index = 1; index < src.Size(); ++index) {
    const Value& item = *src.TryGet(index);
    if (compare_values_for_sort(item, *best) < 0) {
      best = &item;
    }
  }
  return *best;
}

// arg = sequence
[[nodiscard]] inline auto TupleMax(Value arg) -> Value {
  const auto& src = as_array(arg);
  if (src.Size() == 0) {
    throw std::invalid_argument{"TupleMax expects non-empty tuple"};
  }
  const Value& first = *src.TryGet(0);
  const Value* best = &first;
  for (std::size_t index = 1; index < src.Size(); ++index) {
    const Value& item = *src.TryGet(index);
    if (compare_values_for_sort(item, *best) > 0) {
      best = &item;
    }
  }
  return *best;
}

// arg = [sequence, initial, func_ref]
[[nodiscard]] inline auto TupleReduce(Value arg) -> Value {
  auto& src = as_array(array_at(arg, 0));
  Value accumulator = std::move(array_at(arg, 1));
  const Value& reducer = array_at(arg, 2);
  const RegisteredCallable reducer_callable = resolve_registered_callable_ref(reducer);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    accumulator = invoke_binary_callable(reducer_callable, std::move(accumulator), std::move(src[index]));
  }
  return accumulator;
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFindIndex(Value arg) -> Value {
  auto& src = as_array(array_at(arg, 0));
  const Value& pred = array_at(arg, 1);
  const RuntimeCallable predicate = resolve_callable_ref(pred);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (as_bool(predicate(std::move(src[index])))) {
      return make_int(static_cast<Int>(index));
    }
  }
  return make_int(-1);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAny(Value arg) -> Value {
  auto& src = as_array(array_at(arg, 0));
  const Value& pred = array_at(arg, 1);
  const RuntimeCallable predicate = resolve_callable_ref(pred);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (as_bool(predicate(std::move(src[index])))) {
      return make_bool(true);
    }
  }
  return make_bool(false);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAll(Value arg) -> Value {
  auto& src = as_array(array_at(arg, 0));
  const Value& pred = array_at(arg, 1);
  const RuntimeCallable predicate = resolve_callable_ref(pred);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    if (!as_bool(predicate(std::move(src[index])))) {
      return make_bool(false);
    }
  }
  return make_bool(true);
}

[[nodiscard]] inline auto tuple_range_impl(const Int start, const Int stop, const Int step) -> Value {
  if (step == 0) {
    throw std::invalid_argument{"TupleRange step cannot be 0"};
  }

  Array out;
  if (step > 0) {
    for (Int current = start; current < stop; current += step) {
      out.PushBack(make_int(current));
    }
  } else {
    for (Int current = start; current > stop; current += step) {
      out.PushBack(make_int(current));
    }
  }
  return Value{std::move(out)};
}

// arg = stop or [stop]
[[nodiscard]] inline auto TupleRangeInt64(Value arg) -> Value {
  const Int stop = arg.HasArray() ? as_int_value(array_at(arg, 0)) : as_int_value(arg);
  return tuple_range_impl(0, stop, 1);
}

// arg = [start, stop]
[[nodiscard]] inline auto TupleRangeInt64Int64(Value arg) -> Value {
  return tuple_range_impl(as_int_value(array_at(arg, 0)), as_int_value(array_at(arg, 1)), 1);
}

// arg = [start, stop, step]
[[nodiscard]] inline auto TupleRangeInt64Int64Int64(Value arg) -> Value {
  return tuple_range_impl(as_int_value(array_at(arg, 0)), as_int_value(array_at(arg, 1)), as_int_value(array_at(arg, 2)));
}

}  // namespace fleaux::runtime
