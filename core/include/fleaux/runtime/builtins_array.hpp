#pragma once
// Multidimensional array builtins (SetAt2D, Fill, Transpose2D, Slice2D, Reshape).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Multidimensional array builtins

template <typename Visitor>
void for_each_array_value(const TupleView arr, Visitor&& visitor) {
  for (std::size_t element_index = 0; element_index < arr.size(); ++element_index) {
    visitor(arr[element_index]);
  }
}

inline void append_array_range(TupleBuilder& out, const TupleView src, const std::size_t start,
                               const std::size_t stop) {
  for (std::size_t element_index = start; element_index < stop; ++element_index) {
    out.push(src[element_index]);
  }
}

[[nodiscard]] inline auto validated_array_value(const Value& value, const std::string_view builtin_name) -> TupleView {
  if (!value.is_tuple()) {
    throw std::logic_error(std::format("{} invariant violated: expected validated tuple value", builtin_name));
  }
  return value.tuple();
}

[[nodiscard]] inline auto validated_grid_value(const Value& value, const std::string_view builtin_name) -> TupleView {
  const auto& grid = validated_array_value(value, builtin_name);
  for (std::size_t row_index = 0; row_index < grid.size(); ++row_index) {
    if (!grid[row_index].is_tuple()) {
      throw std::logic_error(std::format("{} invariant violated: expected validated tuple row", builtin_name));
    }
  }
  return grid;
}

// A Tuple<UInt64...> of indices or extents, read without checking the kind of each item (guaranteed by its type).
[[nodiscard]] inline auto uint64_indices(const Value& value) -> std::vector<std::size_t> {
  const auto& tuple = as_tuple(value);
  std::vector<std::size_t> indices;
  indices.reserve(tuple.size());
  for (std::size_t index_position = 0; index_position < tuple.size(); ++index_position) {
    indices.push_back(uint64_index(tuple[index_position]));
  }
  return indices;
}

[[nodiscard]] inline auto array_shape(const Value& value) -> std::optional<std::vector<std::size_t>> {
  if (!value.is_tuple()) {
    return std::vector<std::size_t>{};
  }

  const auto& arr = as_tuple(value);
  std::vector<std::size_t> shape;
  shape.push_back(arr.size());
  if (arr.size() == 0) {
    return shape;
  }

  auto first_child_shape = array_shape(arr[0]);
  if (!first_child_shape.has_value()) {
    return std::nullopt;
  }
  for (std::size_t child_index = 1; child_index < arr.size(); ++child_index) {
    if (auto child_shape = array_shape(arr[child_index]);
        !child_shape.has_value() || child_shape.value() != first_child_shape.value()) {
      return std::nullopt;
    }
  }

  shape.insert(shape.end(), first_child_shape->begin(), first_child_shape->end());
  return shape;
}

// The number of items flatten_into puts out for `value`.
[[nodiscard]] inline auto count_array_leaves(const Value& value) -> std::size_t {
  if (!value.is_tuple()) {
    return 1U;
  }
  std::size_t count = 0;
  for (const Value& item : value.tuple()) {
    count += count_array_leaves(item);
  }
  return count;
}

inline void flatten_into(const Value& value, TupleBuilder& out) {
  if (!value.is_tuple()) {
    out.push(value);
    return;
  }
  for_each_array_value(value.tuple(), [&](const Value& item) { flatten_into(item, out); });
}

// Replaces the item at `path`, from `depth` on, inside `value`. The tuples along the path change in place where the
// caller holds the only reference to them (mutable_tuple), and are copied where they are shared.
inline void set_at_path(Value& value, const std::vector<std::size_t>& path, const std::size_t depth, Value replacement) {
  if (depth == path.size()) {
    value = std::move(replacement);
    return;
  }
  const std::size_t size = as_tuple(value).size();
  const std::size_t idx = path.at(depth);
  if (idx >= size) {
    throw std::out_of_range(
        std::format("Array.SetAtND: index {} out of range at depth {} for size {}", idx, depth, size));
  }
  set_at_path(value.mutable_tuple()[idx], path, depth + 1, std::move(replacement));
}

[[nodiscard]] inline auto reshape_from_flat(const TupleView flat, const std::vector<std::size_t>& dims,
                                            const std::size_t depth, std::size_t& cursor) -> Value {
  if (depth == dims.size()) {
    return flat[cursor++];
  }

  TupleBuilder out(dims.at(depth));
  for (std::size_t element_index = 0; element_index < dims.at(depth); ++element_index) {
    out.push(reshape_from_flat(flat, dims, depth + 1, cursor));
  }
  return std::move(out).finish();
}

// arg = [array, index]
[[nodiscard]] inline auto ArrayGetAt(const Arguments& args) -> Value {
  const auto& arr = validated_array_value(args[0], "Std.Array.GetAt");
  const std::size_t idx = uint64_index(args[1]);
  if (idx >= arr.size()) {
    throw std::out_of_range(std::format("ArrayGetAt: index {} out of range for size {}", idx, arr.size()));
  }
  return arr[idx];
}

// The builtins below that return a changed tuple change it in place when the argument holds the only reference to
// it: take_tuple_item moves it out of the argument, and mutable_tuple copies it only when it is shared.

// arg = [array, index, value]
[[nodiscard]] inline auto ArraySetAt(const Arguments& args) -> Value {
  const std::size_t idx = uint64_index(args[1]);
  Value array = args.take(0);
  if (const std::size_t size = validated_array_value(array, "Std.Array.SetAt").size(); idx >= size) {
    throw std::out_of_range(std::format("ArraySetAt: index {} out of range for size {}", idx, size));
  }
  array.mutable_tuple()[idx] = args.take(2);
  return array;
}

// arg = [array, index, value]
[[nodiscard]] inline auto ArrayInsertAt(const Arguments& args) -> Value {
  const std::size_t idx = uint64_index(args[1]);
  Value array = args.take(0);
  const std::size_t size = validated_array_value(array, "Std.Array.InsertAt").size();
  if (idx > size) {
    throw std::out_of_range(std::format("ArrayInsertAt: index {} out of range for size {}", idx, size));
  }
  TupleBuilder out = TupleBuilder::reopen(std::move(array), size + 1U);
  out.insert(idx, args.take(2));
  return std::move(out).finish();
}

// arg = [array, index]
[[nodiscard]] inline auto ArrayRemoveAt(const Arguments& args) -> Value {
  const std::size_t idx = uint64_index(args[1]);
  Value array = args.take(0);
  if (const std::size_t size = validated_array_value(array, "Std.Array.RemoveAt").size(); idx >= size) {
    throw std::out_of_range(std::format("ArrayRemoveAt: index {} out of range for size {}", idx, size));
  }
  return remove_tuple_items(std::move(array), idx, idx + 1U);
}

// arg = [array, start, stop], stop exclusive
[[nodiscard]] inline auto ArraySlice(const Arguments& args) -> Value {
  const std::size_t start = uint64_index(args[1]);
  const std::size_t stop = uint64_index(args[2]);
  Value array = args.take(0);
  const std::size_t size = validated_array_value(array, "Std.Array.Slice").size();
  if (start > stop) {
    throw std::invalid_argument(std::format("ArraySlice: start {} > stop {}", start, stop));
  }
  if (stop > size) {
    throw std::out_of_range(std::format("ArraySlice: stop {} out of range for size {}", stop, size));
  }
  return keep_tuple_items(std::move(array), start, stop);
}

// arg = [lhs, rhs]
[[nodiscard]] inline auto ArrayConcat(const Arguments& args) -> Value {
  Value lhs = args.take(0);
  Value rhs = args.take(1);
  const std::size_t total =
      validated_array_value(lhs, "Std.Array.Concat").size() + validated_array_value(rhs, "Std.Array.Concat").size();
  // The left tuple grows in its own block when the argument held it once; the right one's items move when it did too.
  TupleBuilder out = TupleBuilder::reopen(std::move(lhs), total);
  push_tuple_items(out, std::move(rhs));
  return std::move(out).finish();
}

// arg = [grid, row, col, value]
// grid is a tuple of tuples (rows x cols)
// Returns a new grid with element at (row, col) replaced.
[[nodiscard]] inline auto ArraySetAt2D(const Arguments& args) -> Value {
  const std::size_t row_idx = uint64_index(args[1]);
  const std::size_t col_idx = uint64_index(args[2]);
  Value grid = args.take(0);
  const TupleView rows = validated_array_value(grid, "Std.Array.SetAt2D");
  if (row_idx >= rows.size()) {
    throw std::out_of_range(std::format("ArraySetAt2D: row {} out of range for {} rows", row_idx, rows.size()));
  }
  if (const auto& target_row = validated_array_value(rows[row_idx], "Std.Array.SetAt2D");
      col_idx >= target_row.size()) {
    throw std::out_of_range(
        std::format("ArraySetAt2D: col {} out of range for row size {}", col_idx, target_row.size()));
  }

  // The row changes in place too when the grid held the only reference to it.
  Value& row = grid.mutable_tuple()[row_idx];
  row.mutable_tuple()[col_idx] = args.take(3);
  return grid;
}

// arg = [tuple, start_index, length, value]
// Returns a new tuple with elements [start_index, start_index+length) set to value.
[[nodiscard]] inline auto ArrayFill(const Arguments& args) -> Value {
  const std::size_t start_idx = uint64_index(args[1]);
  const std::size_t fill_len = uint64_index(args[2]);
  Value array = args.take(0);
  const std::size_t size = validated_array_value(array, "Std.Array.Fill").size();

  if (start_idx > size) {
    throw std::out_of_range(std::format("ArrayFill: start_index {} exceeds tuple size {}", start_idx, size));
  }
  if (const std::size_t max_len = size - start_idx; fill_len > max_len) {
    throw std::out_of_range(
        std::format("ArrayFill: length {} exceeds available range {} from start {}", fill_len, max_len, start_idx));
  }

  const Value& fill_val = args[3];
  std::ranges::fill(array.mutable_tuple().subspan(start_idx, fill_len), fill_val);
  return array;
}

// arg = grid (tuple of tuples)
// Returns a new grid where rows become columns and columns become rows.
[[nodiscard]] inline auto ArrayTranspose2D(Value arg) -> Value {
  const auto& grid = validated_grid_value(arg, "Std.Array.Transpose2D");
  if (grid.size() == 0) {
    return make_tuple();
  }

  const auto& first_row = validated_array_value(grid[0], "Std.Array.Transpose2D");
  const std::size_t num_cols = first_row.size();
  const std::size_t num_rows = grid.size();

  for (std::size_t row_index = 1; row_index < num_rows; ++row_index) {
    if (const auto& row = validated_array_value(grid[row_index], "Std.Array.Transpose2D");
        row.size() != num_cols) {
      throw std::invalid_argument(std::format("ArrayTranspose2D: ragged grid detected at row {}", row_index));
    }
  }

  TupleBuilder out_grid(num_cols);
  for (std::size_t column_index = 0; column_index < num_cols; ++column_index) {
    TupleBuilder new_row(num_rows);
    for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
      const auto& row = validated_array_value(grid[row_index], "Std.Array.Transpose2D");
      new_row.push(row[column_index]);
    }
    out_grid.push(std::move(new_row).finish());
  }

  return std::move(out_grid).finish();
}

// arg = [grid, row_start, row_end, col_start, col_end]
// Returns a new grid containing elements in [row_start, row_end) x [col_start, col_end).
[[nodiscard]] inline auto ArraySlice2D(const Arguments& args) -> Value {
  const auto& grid = validated_grid_value(args[0], "Std.Array.Slice2D");
  const std::size_t row_start =
      uint64_index(args[1]);
  const std::size_t row_end = uint64_index(args[2]);
  const std::size_t col_start =
      uint64_index(args[3]);
  const std::size_t col_end = uint64_index(args[4]);

  if (row_start > row_end) {
    throw std::invalid_argument(std::format("ArraySlice2D: row_start {} > row_end {}", row_start, row_end));
  }
  if (col_start > col_end) {
    throw std::invalid_argument(std::format("ArraySlice2D: col_start {} > col_end {}", col_start, col_end));
  }
  if (row_end > grid.size()) {
    throw std::out_of_range(std::format("ArraySlice2D: row_end {} exceeds grid size {}", row_end, grid.size()));
  }

  TupleBuilder out_grid(row_end - row_start);
  for (std::size_t row_index = row_start; row_index < row_end; ++row_index) {
    const auto& row = validated_array_value(grid[row_index], "Std.Array.Slice2D");
    if (col_end > row.size()) {
      throw std::out_of_range(
          std::format("ArraySlice2D: col_end {} exceeds row {} size {}", col_end, row_index, row.size()));
    }

    TupleBuilder new_row(col_end - col_start);
    for (std::size_t column_index = col_start; column_index < col_end; ++column_index) {
      new_row.push(row[column_index]);
    }
    out_grid.push(std::move(new_row).finish());
  }

  return std::move(out_grid).finish();
}

// arg = [flat_array, rows, cols]
// Reshapes a flattened array into a grid of rows x cols in row-major order.
[[nodiscard]] inline auto ArrayReshape(const Arguments& args) -> Value {
  const auto& flat = validated_array_value(args[0], "Std.Array.Reshape");
  const std::size_t num_rows = uint64_index(args[1]);
  const std::size_t num_cols = uint64_index(args[2]);

  if (num_cols != 0 && num_rows > (std::numeric_limits<std::size_t>::max() / num_cols)) {
    throw std::invalid_argument("ArrayReshape: rows * cols overflows size_t");
  }
  if (const std::size_t expected_size = num_rows * num_cols; flat.size() != expected_size) {
    throw std::invalid_argument(std::format("ArrayReshape: flat_array has {} elements, but {}x{} requires {}",
                                            flat.size(), num_rows, num_cols, expected_size));
  }

  TupleBuilder out_grid(num_rows);
  for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
    TupleBuilder new_row(num_cols);
    for (std::size_t column_index = 0; column_index < num_cols; ++column_index) {
      const std::size_t flat_idx = row_index * num_cols + column_index;
      new_row.push(flat[flat_idx]);
    }
    out_grid.push(std::move(new_row).finish());
  }

  return std::move(out_grid).finish();
}

// arg = value
[[nodiscard]] inline auto ArrayRank(Value arg) -> Value {
  const auto shape = array_shape(arg);
  if (!shape.has_value()) {
    throw std::invalid_argument("ArrayRank: value is not rectangular");
  }
  return make_int(static_cast<Int>(shape->size()));
}

// arg = value
[[nodiscard]] inline auto ArrayShape(Value arg) -> Value {
  const auto shape = array_shape(arg);
  if (!shape.has_value()) {
    throw std::invalid_argument("ArrayShape: value is not rectangular");
  }

  TupleBuilder out(shape->size());
  for (const std::size_t dim : *shape) {
    out.push(make_uint(static_cast<UInt>(dim)));
  }
  return std::move(out).finish();
}

// arg = value
[[nodiscard]] inline auto ArrayFlatten(Value arg) -> Value {
  TupleBuilder out(count_array_leaves(arg));
  flatten_into(arg, out);
  return std::move(out).finish();
}

// arg = [value, indices]
[[nodiscard]] inline auto ArrayGetAtND(const Arguments& args) -> Value {
  const Value* current = &args[0];
  const auto indices = uint64_indices(args[1]);

  for (std::size_t depth = 0; depth < indices.size(); ++depth) {
    const auto& arr = as_tuple(*current);
    const std::size_t idx = indices.at(depth);
    if (idx >= arr.size()) {
      throw std::out_of_range(
          std::format("ArrayGetAtND: index {} out of range at depth {} for size {}", idx, depth, arr.size()));
    }
    current = &arr[idx];
  }
  return *current;
}

// arg = [value, indices, replacement]
[[nodiscard]] inline auto ArraySetAtND(const Arguments& args) -> Value {
  const auto path = uint64_indices(args[1]);
  Value value = args.take(0);
  set_at_path(value, path, 0, args.take(2));
  return value;
}

// arg = [flat_array, shape]
[[nodiscard]] inline auto ArrayReshapeND(const Arguments& args) -> Value {
  const auto& flat = validated_array_value(args[0], "Std.Array.ReshapeND");
  const auto dims =
      uint64_indices(args[1]);

  std::size_t total = 1;
  for (std::size_t shape_index = 0; shape_index < dims.size(); ++shape_index) {
    const std::size_t dim = dims.at(shape_index);
    if (dim != 0 && total > (std::numeric_limits<std::size_t>::max() / dim)) {
      throw std::invalid_argument("ArrayReshapeND: shape product overflows size_t");
    }
    total *= dim;
  }

  if (flat.size() != total) {
    throw std::invalid_argument(
        std::format("ArrayReshapeND: flat array size {} does not match shape product {}", flat.size(), total));
  }

  if (dims.empty()) {
    if (flat.size() != 1) {
      throw std::invalid_argument("ArrayReshapeND: scalar reshape requires exactly 1 flat element");
    }
    return flat[0];
  }

  std::size_t cursor = 0;
  return reshape_from_flat(flat, dims, 0, cursor);
}

}  // namespace fleaux::runtime
