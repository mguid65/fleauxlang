#pragma once
// Tuple collection builtins (Map, Filter, Sort, Reduce, Range, Zip, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.

#include <algorithm>
#include <utility>
#include <vector>

#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

[[nodiscard]] inline auto validated_tuple_value(const Value& value, const std::string_view builtin_name) -> TupleView {
  if (!value.is_tuple()) {
    throw std::logic_error(std::format("{} invariant violated: expected validated tuple value", builtin_name));
  }
  return value.tuple();
}

[[nodiscard]] inline auto validated_callable_ref(const Value& value, const std::string_view builtin_name)
    -> const Callable& {
  if (const Callable* callable = value.callable_if_function(); callable != nullptr) {
    return *callable;
  }
  throw std::logic_error(std::format("{} invariant violated: expected validated callable reference", builtin_name));
}

template <typename Visitor>
void for_each_tuple_value(const TupleView src, Visitor&& visitor) {
  for (std::size_t index = 0; index < src.size(); ++index) {
    visitor(src[index]);
  }
}

template <typename Predicate>
[[nodiscard]] auto tuple_find_index_if(const TupleView src, Predicate&& predicate) -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < src.size(); ++index) {
    if (const Value& item = src[index]; predicate(item)) {
      return index;
    }
  }
  return std::nullopt;
}

template <typename Predicate>
[[nodiscard]] auto tuple_any_of(const TupleView src, Predicate&& predicate) -> bool {
  return tuple_find_index_if(src, predicate).has_value();
}

template <typename Predicate>
[[nodiscard]] auto tuple_all_of(const TupleView src, Predicate&& predicate) -> bool {
  for (std::size_t index = 0; index < src.size(); ++index) {
    if (const Value& item = src[index]; !predicate(item)) {
      return false;
    }
  }
  return true;
}

// Tuple builtins

// arg = [sequence, item]
[[nodiscard]] inline auto TupleAppend(const Arguments& args) -> Value {
  Value sequence = args.take(0);
  const std::size_t size = as_tuple(sequence).size();
  // A sequence the argument held once grows in its own block, which doubles when it is full.
  TupleBuilder out = TupleBuilder::reopen(std::move(sequence), size + 1U);
  out.push(args.take(1));
  return std::move(out).finish();
}

// arg = [sequence, item]  ->  [item, ...sequence]
[[nodiscard]] inline auto TuplePrepend(const Arguments& args) -> Value {
  Value sequence = args.take(0);
  const std::size_t size = as_tuple(sequence).size();
  TupleBuilder out = TupleBuilder::reopen(std::move(sequence), size + 1U);
  out.insert(0U, args.take(1));
  return std::move(out).finish();
}

// arg = sequence
[[nodiscard]] inline auto TupleReverse(Value arg) -> Value {
  const auto& src = validated_tuple_value(arg, "Std.Tuple.Reverse");
  TupleBuilder out(src.size());
  for (std::size_t reverse_index = src.size(); reverse_index > 0; --reverse_index) {
    out.push(src[reverse_index - 1]);
  }
  return std::move(out).finish();
}

// arg = [sequence, item]
[[nodiscard]] inline auto TupleContains(const Arguments& args) -> Value {
  const auto& src = as_tuple(args[0]);
  const Value& item = args[1];
  return make_bool(tuple_any_of(src, [&](const Value& candidate) -> bool { return candidate == item; }));
}

// arg = [sequenceA, sequenceB]  ->  [(a0,b0), (a1,b1), ...]
[[nodiscard]] inline auto TupleZip(const Arguments& args) -> Value {
  const auto& lhs_arr = as_tuple(args[0]);
  const auto& rhs_arr = as_tuple(args[1]);
  const std::size_t min_size = std::min(lhs_arr.size(), rhs_arr.size());
  TupleBuilder out(min_size);
  for (std::size_t index = 0; index < min_size; ++index) {
    out.push(make_tuple(lhs_arr[index], rhs_arr[index]));
  }
  return std::move(out).finish();
}

// arg = [sequence, func_ref]
[[nodiscard]] inline auto TupleMap(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Map");
  const Callable& function = validated_callable_ref(args[1], "Std.Tuple.Map");

  TupleBuilder out(src.size());
  for (const Value& item : src) {
    out.push(function.call(item));
  }
  return std::move(out).finish();
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFilter(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Filter");
  const Callable& predicate = validated_callable_ref(args[1], "Std.Tuple.Filter");

  TupleBuilder out(src.size());
  for (std::size_t index = 0; index < src.size(); ++index) {
    Value item = src[index];
    if (as_bool(predicate.call(item))) {
      out.push(std::move(item));
    }
  }
  return std::move(out).finish();
}

// arg = sequence
[[nodiscard]] inline auto TupleSort(Value arg) -> Value {
  (void)validated_tuple_value(arg, "Std.Tuple.Sort");
  // A tuple held only by the argument is sorted in place; mutable_tuple copies a shared one first.
  std::ranges::stable_sort(
      arg.mutable_tuple(), [](const Value& lhs, const Value& rhs) -> bool { return compare_values_for_sort(lhs, rhs) < 0; });
  return arg;
}

// arg = sequence
[[nodiscard]] inline auto TupleUnique(Value arg) -> Value {
  const auto& src = validated_tuple_value(arg, "Std.Tuple.Unique");
  TupleBuilder out(src.size());
  for (std::size_t index = 0; index < src.size(); ++index) {
    const Value& item = src[index];
    if (const std::span<Value> kept = out.items(); std::ranges::find(kept, item) == kept.end()) {
      out.push(item);
    }
  }
  return std::move(out).finish();
}

// arg = sequence
[[nodiscard]] inline auto TupleMin(Value arg) -> Value {
  const auto& src = validated_tuple_value(arg, "Std.Tuple.Min");
  if (src.size() == 0) {
    throw std::invalid_argument{"the tuple is empty"};
  }
  const Value& first = src[0];
  const Value* best = &first;
  for (std::size_t index = 1; index < src.size(); ++index) {
    const Value& item = src[index];
    if (compare_values_for_sort(item, *best) < 0) {
      best = &item;
    }
  }
  return *best;
}

// arg = sequence
[[nodiscard]] inline auto TupleMax(Value arg) -> Value {
  const auto& src = validated_tuple_value(arg, "Std.Tuple.Max");
  if (src.size() == 0) {
    throw std::invalid_argument{"the tuple is empty"};
  }
  const Value& first = src[0];
  const Value* best = &first;
  for (std::size_t index = 1; index < src.size(); ++index) {
    const Value& item = src[index];
    if (compare_values_for_sort(item, *best) > 0) {
      best = &item;
    }
  }
  return *best;
}

// arg = [sequence, initial, func_ref]
[[nodiscard]] inline auto TupleReduce(const Arguments& args) -> Value {
  Value accumulator = args.take(1);
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Reduce");
  const Callable& reducer = validated_callable_ref(args[2], "Std.Tuple.Reduce");
  for (const Value& item : src) {
    accumulator = reducer.call_binary(std::move(accumulator), item);
  }
  return accumulator;
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleFindIndex(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.FindIndex");
  const Callable& predicate = validated_callable_ref(args[1], "Std.Tuple.FindIndex");
  for (std::size_t index = 0; index < src.size(); ++index) {
    if (as_bool(predicate.call(src[index]))) {
      return ResultOk(make_int(static_cast<Int>(index)));
    }
  }
  return ResultErr(make_string("Std.Tuple.FindIndex: no item matches"));
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAny(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Any");
  const Callable& predicate = validated_callable_ref(args[1], "Std.Tuple.Any");
  for (std::size_t index = 0; index < src.size(); ++index) {
    if (as_bool(predicate.call(src[index]))) {
      return make_bool(true);
    }
  }
  return make_bool(false);
}

// arg = [sequence, pred_ref]
[[nodiscard]] inline auto TupleAll(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.All");
  const Callable& predicate = validated_callable_ref(args[1], "Std.Tuple.All");
  for (std::size_t index = 0; index < src.size(); ++index) {
    if (!as_bool(predicate.call(src[index]))) {
      return make_bool(false);
    }
  }
  return make_bool(true);
}

[[nodiscard]] inline auto tuple_range_impl(const Int start, const Int stop, const Int step) -> Value {
  if (step == 0) {
    throw std::invalid_argument{"step cannot be 0"};
  }

  // The count of items, in unsigned arithmetic, where no difference of two Int64 values overflows.
  UInt items = 0;
  if (step > 0 && start < stop) {
    items = (static_cast<UInt>(stop) - static_cast<UInt>(start) - 1U) / static_cast<UInt>(step) + 1U;
  } else if (step < 0 && start > stop) {
    items = (static_cast<UInt>(start) - static_cast<UInt>(stop) - 1U) / (UInt{0} - static_cast<UInt>(step)) + 1U;
  }
  const std::size_t count = to_index(items);
  TupleBuilder out(count);
  Int current = start;
  for (std::size_t index = 0; index < count; ++index) {
    out.push(make_int(current));
    if (index + 1U < count) {
      current += step;
    }
  }
  return std::move(out).finish();
}

// arg = stop or [stop]
[[nodiscard]] inline auto TupleRangeInt64(Value arg) -> Value {
  const Int stop = int64_value(arg);
  return tuple_range_impl(0, stop, 1);
}

// arg = [start, stop]
[[nodiscard]] inline auto TupleRangeInt64Int64(const Arguments& args) -> Value {
  return tuple_range_impl(int64_value(args[0]),
                          int64_value(args[1]), 1);
}

// arg = [start, stop, step]
[[nodiscard]] inline auto TupleRangeInt64Int64Int64(const Arguments& args) -> Value {
  return tuple_range_impl(int64_value(args[0]),
                          int64_value(args[1]),
                          int64_value(args[2]));
}

// Changes to one item, and the joining of two tuples. A tuple the argument holds alone changes in place:
// take_tuple_item moves it out of the argument, and mutable_tuple copies it only when it is shared.

// arg = [array, index, value]
[[nodiscard]] inline auto TupleSetAt(const Arguments& args) -> Value {
  const std::size_t idx = count_index(args[1], "index");
  Value array = args.take(0);
  if (const std::size_t size = validated_tuple_value(array, "Std.Tuple.SetAt").size(); idx >= size) {
    throw std::out_of_range(std::format("index {} out of range for size {}", idx, size));
  }
  array.mutable_tuple()[idx] = args.take(2);
  return array;
}

// arg = [array, index, value]
[[nodiscard]] inline auto TupleInsertAt(const Arguments& args) -> Value {
  const std::size_t idx = count_index(args[1], "index");
  Value array = args.take(0);
  const std::size_t size = validated_tuple_value(array, "Std.Tuple.InsertAt").size();
  if (idx > size) {
    throw std::out_of_range(std::format("index {} out of range for size {}", idx, size));
  }
  TupleBuilder out = TupleBuilder::reopen(std::move(array), size + 1U);
  out.insert(idx, args.take(2));
  return std::move(out).finish();
}

// arg = [array, index]
[[nodiscard]] inline auto TupleRemoveAt(const Arguments& args) -> Value {
  const std::size_t idx = count_index(args[1], "index");
  Value array = args.take(0);
  if (const std::size_t size = validated_tuple_value(array, "Std.Tuple.RemoveAt").size(); idx >= size) {
    throw std::out_of_range(std::format("index {} out of range for size {}", idx, size));
  }
  return remove_tuple_items(std::move(array), idx, idx + 1U);
}

// arg = [lhs, rhs]
[[nodiscard]] inline auto TupleConcat(const Arguments& args) -> Value {
  Value lhs = args.take(0);
  Value rhs = args.take(1);
  const std::size_t total = validated_tuple_value(lhs, "Std.Tuple.Concat").size() + validated_tuple_value(rhs, "Std.Tuple.Concat").size();
  // The left tuple grows in its own block when the argument held it once; the right one's items move when it did too.
  TupleBuilder out = TupleBuilder::reopen(std::move(lhs), total);
  push_tuple_items(out, std::move(rhs));
  return std::move(out).finish();
}

// More over tuples. Each calls its function from C++, once per item, in order.

// arg = sequence -> ((0, item0), (1, item1), ...)
[[nodiscard]] inline auto TupleEnumerate(Value arg) -> Value {
  const auto& src = validated_tuple_value(arg, "Std.Tuple.Enumerate");
  TupleBuilder out(src.size());
  for (std::size_t index = 0; index < src.size(); ++index) {
    out.push(make_tuple(make_int(static_cast<Int>(index)), Value{src[index]}));
  }
  return std::move(out).finish();
}

// args = [sequence, key_func]: the items in the order of their keys, items with equal keys in their order. Each key
// is computed once.
[[nodiscard]] inline auto TupleSortBy(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.SortBy");
  const Callable& key_of = validated_callable_ref(args[1], "Std.Tuple.SortBy");
  std::vector<std::pair<Value, std::size_t>> keyed;
  keyed.reserve(src.size());
  for (std::size_t index = 0; index < src.size(); ++index) {
    keyed.emplace_back(key_of.call(src[index]), index);
  }
  std::ranges::stable_sort(keyed,
                           [](const auto& lhs, const auto& rhs) -> bool { return compare_values_for_sort(lhs.first, rhs.first) < 0; });
  TupleBuilder out(src.size());
  for (const auto& [key, index] : keyed) {
    out.push(src[index]);
  }
  return std::move(out).finish();
}

// args = [sequence, key_func]: a dict from each key to the items that have it, in the order the keys first appear.
[[nodiscard]] inline auto TupleGroupBy(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.GroupBy");
  const Callable& key_of = validated_callable_ref(args[1], "Std.Tuple.GroupBy");
  Value group_of_key = make_dict();
  std::vector<Value> keys;
  std::vector<std::vector<Value>> groups;
  for (const Value& item : src) {
    Value key = key_of.call(item);
    if (const Value* group = group_of_key.dict().find(key); group != nullptr) {
      groups[static_cast<std::size_t>(group->int_value())].push_back(item);
      continue;
    }
    group_of_key.mutable_dict().set(key, make_int(static_cast<Int>(groups.size())));
    keys.push_back(std::move(key));
    groups.push_back({item});
  }
  Value out = make_dict();
  Dict& grouped = out.mutable_dict();
  grouped.reserve(keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    TupleBuilder members(groups[index].size());
    for (Value& member : groups[index]) {
      members.push(std::move(member));
    }
    grouped.set(std::move(keys[index]), std::move(members).finish());
  }
  return out;
}

// args = [sequence, pred] -> (the items pred holds for, the others)
[[nodiscard]] inline auto TuplePartition(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Partition");
  const Callable& predicate = validated_callable_ref(args[1], "Std.Tuple.Partition");
  TupleBuilder kept(src.size());
  TupleBuilder rest(src.size());
  for (const Value& item : src) {
    (as_bool(predicate.call(item)) ? kept : rest).push(item);
  }
  return make_tuple(std::move(kept).finish(), std::move(rest).finish());
}

// args = [sequence, func]: the items of every tuple func gives, in order.
[[nodiscard]] inline auto TupleFlatMap(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.FlatMap");
  const Callable& function = validated_callable_ref(args[1], "Std.Tuple.FlatMap");
  TupleBuilder out(src.size());
  for (const Value& item : src) {
    push_tuple_items(out, function.call(item));
  }
  return std::move(out).finish();
}

// args = [sequence, initial, func] -> the accumulator after each item: (func(initial, item0), func(that, item1), ...)
[[nodiscard]] inline auto TupleScan(const Arguments& args) -> Value {
  const auto& src = validated_tuple_value(args[0], "Std.Tuple.Scan");
  const Value& function = args[2];
  (void)validated_callable_ref(function, "Std.Tuple.Scan");
  Value accumulator = args.take(1);
  TupleBuilder out(src.size());
  for (const Value& item : src) {
    accumulator = invoke_binary_callable(function, std::move(accumulator), item);
    out.push(accumulator);
  }
  return std::move(out).finish();
}

// The number of items at the start of `src` that `predicate` holds for.
[[nodiscard]] inline auto leading_count(const TupleView src, const Callable& predicate) -> std::size_t {
  std::size_t count = 0;
  while (count < src.size() && as_bool(predicate.call(src[count]))) {
    ++count;
  }
  return count;
}

// args = [sequence, pred] -> the items up to the first that pred does not hold for
[[nodiscard]] inline auto TupleTakeWhile(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t count =
      leading_count(validated_tuple_value(seq, "Std.Tuple.TakeWhile"), validated_callable_ref(args[1], "Std.Tuple.TakeWhile"));
  return keep_tuple_items(std::move(seq), 0U, count);
}

// args = [sequence, pred] -> the items from the first that pred does not hold for
[[nodiscard]] inline auto TupleDropWhile(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const auto& src = validated_tuple_value(seq, "Std.Tuple.DropWhile");
  const std::size_t size = src.size();
  const std::size_t count = leading_count(src, validated_callable_ref(args[1], "Std.Tuple.DropWhile"));
  return keep_tuple_items(std::move(seq), count, size);
}

}  // namespace fleaux::runtime
