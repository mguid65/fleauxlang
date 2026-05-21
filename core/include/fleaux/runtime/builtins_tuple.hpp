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
  const auto& args = require_args(arg, 2, "TupleAppend");
  const auto& src = as_array(*args.TryGet(0));
  Array out;
  out.Reserve(src.Size() + 1);
  for (std::size_t index = 0; index < src.Size(); ++index) {
    out.PushBack(*src.TryGet(index));
  }
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
  const auto& args = require_args(arg, 2, "TupleContains");
  const auto& src = as_array(*args.TryGet(0));
  const Value& item = *args.TryGet(1);
  return make_bool(tuple_any_of(src, [&](const Value& candidate) -> bool { return candidate == item; }));
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
  for_each_tuple_value(src, [&](const Value& item) { out.PushBack(invoke_callable_ref(function_ref, item)); });
  return Value{std::move(out)};
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFilter(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleFilter");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);

  Array out;
  for_each_tuple_value(src, [&](const Value& item) {
    if (as_bool(invoke_callable_ref(pred, item))) {
      out.PushBack(item);
    }
  });
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
  const auto& args = require_args(arg, 3, "TupleReduce");
  const auto& src = as_array(*args.TryGet(0));
  Value accumulator = *args.TryGet(1);
  const Value& reducer = *args.TryGet(2);
  for_each_tuple_value(src, [&](const Value& item) {
    accumulator = invoke_callable_ref(reducer, make_tuple(std::move(accumulator), item));
  });
  return accumulator;
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFindIndex(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleFindIndex");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  if (const auto index =
          tuple_find_index_if(src, [&](const Value& item) -> bool { return as_bool(invoke_callable_ref(pred, item)); });
      index.has_value()) {
    return make_int(static_cast<Int>(*index));
  }
  return make_int(-1);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAny(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleAny");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  return make_bool(
      tuple_any_of(src, [&](const Value& item) -> bool { return as_bool(invoke_callable_ref(pred, item)); }));
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAll(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "TupleAll");
  const auto& src = as_array(*args.TryGet(0));
  const Value& pred = *args.TryGet(1);
  return make_bool(
      tuple_all_of(src, [&](const Value& item) -> bool { return as_bool(invoke_callable_ref(pred, item)); }));
}

// arg = [stop] | [start, stop] | [start, stop, step]
[[nodiscard]] inline auto TupleRange(Value arg) -> Value {
  Int start = 0;
  Int stop = 0;
  Int step = 1;

  if (!arg.HasArray()) {
    stop = as_int_value(arg);
  } else {
    if (const auto& args = as_array(arg); args.Size() == 1) {
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
  }

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

}  // namespace fleaux::runtime
