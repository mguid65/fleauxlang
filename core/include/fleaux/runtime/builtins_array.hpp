#pragma once
// Multidimensional array builtins (SetAt2D, Fill, Transpose2D, Slice2D, Reshape).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Multidimensional array builtins

template <typename Visitor>
void for_each_array_value(const Array& arr, Visitor&& visitor) {
  for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
    visitor(*arr.TryGet(element_index));
  }
}

inline void append_array_range(Array& out, const Array& src, const std::size_t start, const std::size_t stop) {
  for (std::size_t element_index = start; element_index < stop; ++element_index) {
    out.PushBack(*src.TryGet(element_index));
  }
}

[[nodiscard]] inline auto validated_array_value(const Value& value, const std::string_view builtin_name) -> const Array& {
  const auto array = value.TryGetArray();
  if (!array) {
    throw std::logic_error(std::format("{} invariant violated: expected validated tuple value", builtin_name));
  }
  return *array;
}

[[nodiscard]] inline auto validated_grid_value(const Value& value, const std::string_view builtin_name) -> const Array& {
  const auto& grid = validated_array_value(value, builtin_name);
  for (std::size_t row_index = 0; row_index < grid.Size(); ++row_index) {
    if (!grid.TryGet(row_index)->HasArray()) {
      throw std::logic_error(std::format("{} invariant violated: expected validated tuple row", builtin_name));
    }
  }
  return grid;
}

// A Tuple<UInt64...> of indices or extents, read without checking the kind of each item (guaranteed by its type).
[[nodiscard]] inline auto uint64_indices(const Value& value) -> std::vector<std::size_t> {
  const auto& tuple = as_array(value);
  std::vector<std::size_t> indices;
  indices.reserve(tuple.Size());
  for (std::size_t index_position = 0; index_position < tuple.Size(); ++index_position) {
    indices.push_back(uint64_index(*tuple.TryGet(index_position)));
  }
  return indices;
}

[[nodiscard]] inline auto array_shape(const Value& value) -> std::optional<std::vector<std::size_t>> {
  if (!value.HasArray()) {
    return std::vector<std::size_t>{};
  }

  const auto& arr = as_array(value);
  std::vector<std::size_t> shape;
  shape.push_back(arr.Size());
  if (arr.Size() == 0) {
    return shape;
  }

  auto first_child_shape = array_shape(*arr.TryGet(0));
  if (!first_child_shape.has_value()) {
    return std::nullopt;
  }
  for (std::size_t child_index = 1; child_index < arr.Size(); ++child_index) {
    if (auto child_shape = array_shape(*arr.TryGet(child_index));
        !child_shape.has_value() || child_shape.value() != first_child_shape.value()) {
      return std::nullopt;
    }
  }

  shape.insert(shape.end(), first_child_shape->begin(), first_child_shape->end());
  return shape;
}

inline void flatten_into(const Value& value, Array& out) {
  if (!value.HasArray()) {
    out.PushBack(value);
    return;
  }
  const auto& arr = as_array(value);
  for_each_array_value(arr, [&](const Value& item) { flatten_into(item, out); });
}

[[nodiscard]] inline auto set_at_path(const Value& value, const std::vector<std::size_t>& path, const std::size_t depth,
                                      const Value& replacement) -> Value {
  if (depth == path.size()) {
    return replacement;
  }
  const auto& arr = as_array(value);
  const std::size_t idx = path.at(depth);
  if (idx >= arr.Size()) {
    throw std::out_of_range(
        std::format("Array.SetAtND: index {} out of range at depth {} for size {}", idx, depth, arr.Size()));
  }

  Array out;
  out.Reserve(arr.Size());
  for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
    if (element_index == idx) {
      out.PushBack(set_at_path(*arr.TryGet(element_index), path, depth + 1, replacement));
    } else {
      out.PushBack(*arr.TryGet(element_index));
    }
  }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto reshape_from_flat(const Array& flat, const std::vector<std::size_t>& dims,
                                            const std::size_t depth, std::size_t& cursor) -> Value {
  if (depth == dims.size()) {
    return *flat.TryGet(cursor++);
  }

  Array out;
  out.Reserve(dims.at(depth));
  for (std::size_t element_index = 0; element_index < dims.at(depth); ++element_index) {
    out.PushBack(reshape_from_flat(flat, dims, depth + 1, cursor));
  }
  return Value{std::move(out)};
}

// arg = [array, index]
[[nodiscard]] inline auto ArrayGetAt(Value arg) -> Value {
  const auto& arr = validated_array_value(array_at(arg, 0), "Std.Array.GetAt");
  const std::size_t idx = uint64_index(array_at(arg, 1));
  if (idx >= arr.Size()) {
    throw std::out_of_range(std::format("ArrayGetAt: index {} out of range for size {}", idx, arr.Size()));
  }
  return *arr.TryGet(idx);
}

// arg = [array, index, value]
[[nodiscard]] inline auto ArraySetAt(Value arg) -> Value {
  const auto& arr = validated_array_value(array_at(arg, 0), "Std.Array.SetAt");
  const std::size_t idx = uint64_index(array_at(arg, 1));
  const Value& replacement = array_at(arg, 2);
  if (idx >= arr.Size()) {
    throw std::out_of_range(std::format("ArraySetAt: index {} out of range for size {}", idx, arr.Size()));
  }

  Array out;
  out.Reserve(arr.Size());
  for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
    out.PushBack(element_index == idx ? replacement : *arr.TryGet(element_index));
  }
  return Value{std::move(out)};
}

// arg = [array, index, value]
[[nodiscard]] inline auto ArrayInsertAt(Value arg) -> Value {
  const auto& arr = validated_array_value(array_at(arg, 0), "Std.Array.InsertAt");
  const std::size_t idx = uint64_index(array_at(arg, 1));
  const Value& value = array_at(arg, 2);
  if (idx > arr.Size()) {
    throw std::out_of_range(std::format("ArrayInsertAt: index {} out of range for size {}", idx, arr.Size()));
  }

  Array out;
  out.Reserve(arr.Size() + 1);
  append_array_range(out, arr, 0, idx);
  out.PushBack(value);
  append_array_range(out, arr, idx, arr.Size());
  return Value{std::move(out)};
}

// arg = [array, index]
[[nodiscard]] inline auto ArrayRemoveAt(Value arg) -> Value {
  const auto& arr = validated_array_value(array_at(arg, 0), "Std.Array.RemoveAt");
  const std::size_t idx = uint64_index(array_at(arg, 1));
  if (idx >= arr.Size()) {
    throw std::out_of_range(std::format("ArrayRemoveAt: index {} out of range for size {}", idx, arr.Size()));
  }

  Array out;
  out.Reserve(arr.Size() - 1);
  for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
    if (element_index != idx) {
      out.PushBack(*arr.TryGet(element_index));
    }
  }
  return Value{std::move(out)};
}

// arg = [array, start, stop], stop exclusive
[[nodiscard]] inline auto ArraySlice(Value arg) -> Value {
  const auto& arr = validated_array_value(array_at(arg, 0), "Std.Array.Slice");
  const std::size_t start = uint64_index(array_at(arg, 1));
  const std::size_t stop = uint64_index(array_at(arg, 2));
  if (start > stop) {
    throw std::invalid_argument(std::format("ArraySlice: start {} > stop {}", start, stop));
  }
  if (stop > arr.Size()) {
    throw std::out_of_range(std::format("ArraySlice: stop {} out of range for size {}", stop, arr.Size()));
  }

  Array out;
  out.Reserve(stop - start);
  append_array_range(out, arr, start, stop);
  return Value{std::move(out)};
}

// arg = [lhs, rhs]
[[nodiscard]] inline auto ArrayConcat(Value arg) -> Value {
  const auto& lhs = validated_array_value(array_at(arg, 0), "Std.Array.Concat");
  const auto& rhs = validated_array_value(array_at(arg, 1), "Std.Array.Concat");

  Array out;
  out.Reserve(lhs.Size() + rhs.Size());
  append_array_range(out, lhs, 0, lhs.Size());
  append_array_range(out, rhs, 0, rhs.Size());
  return Value{std::move(out)};
}

// arg = [grid, row, col, value]
// grid is a tuple of tuples (rows x cols)
// Returns a new grid with element at (row, col) replaced.
[[nodiscard]] inline auto ArraySetAt2D(Value arg) -> Value {
  const auto& grid = validated_grid_value(array_at(arg, 0), "Std.Array.SetAt2D");
  const std::size_t row_idx = uint64_index(array_at(arg, 1));
  const std::size_t col_idx = uint64_index(array_at(arg, 2));
  const Value& new_val = array_at(arg, 3);

  if (row_idx >= grid.Size()) {
    throw std::out_of_range(std::format("ArraySetAt2D: row {} out of range for {} rows", row_idx, grid.Size()));
  }

  if (const auto& target_row = validated_array_value(*grid.TryGet(row_idx), "Std.Array.SetAt2D");
      col_idx >= target_row.Size()) {
    throw std::out_of_range(
        std::format("ArraySetAt2D: col {} out of range for row size {}", col_idx, target_row.Size()));
  }

  Value out_grid{Array{}};
  auto& out_grid_array = as_array(out_grid);
  out_grid_array.Reserve(grid.Size());
  for (std::size_t row_index = 0; row_index < grid.Size(); ++row_index) {
    const auto& row_val = *grid.TryGet(row_index);
    const auto& row = validated_array_value(row_val, "Std.Array.SetAt2D");
    if (row_index != row_idx) {
      out_grid_array.PushBack(row_val);
      continue;
    }

    Value new_row{Array{}};
    auto& new_row_array = as_array(new_row);
    new_row_array.Reserve(row.Size());
    for (std::size_t column_index = 0; column_index < row.Size(); ++column_index) {
      new_row_array.PushBack(column_index == col_idx ? new_val : *row.TryGet(column_index));
    }
    out_grid_array.PushBack(new_row);
  }

  return out_grid;
}

// arg = [tuple, start_index, length, value]
// Returns a new tuple with elements [start_index, start_index+length) set to value.
[[nodiscard]] inline auto ArrayFill(Value arg) -> Value {
  const auto& src = validated_array_value(array_at(arg, 0), "Std.Array.Fill");
  const std::size_t start_idx = uint64_index(array_at(arg, 1));
  const std::size_t fill_len = uint64_index(array_at(arg, 2));
  const Value& fill_val = array_at(arg, 3);

  if (start_idx > src.Size()) {
    throw std::out_of_range(std::format("ArrayFill: start_index {} exceeds tuple size {}", start_idx, src.Size()));
  }
  if (const std::size_t max_len = src.Size() - start_idx; fill_len > max_len) {
    throw std::out_of_range(
        std::format("ArrayFill: length {} exceeds available range {} from start {}", fill_len, max_len, start_idx));
  }
  const std::size_t end_idx = start_idx + fill_len;

  Array out;
  out.Reserve(src.Size());
  for (std::size_t element_index = 0; element_index < src.Size(); ++element_index) {
    out.PushBack((element_index >= start_idx && element_index < end_idx) ? fill_val : *src.TryGet(element_index));
  }

  return Value{std::move(out)};
}

// arg = grid (tuple of tuples)
// Returns a new grid where rows become columns and columns become rows.
[[nodiscard]] inline auto ArrayTranspose2D(Value arg) -> Value {
  const auto& grid = validated_grid_value(arg, "Std.Array.Transpose2D");
  if (grid.Size() == 0) {
    return Value{Array{}};
  }

  const auto& first_row = validated_array_value(*grid.TryGet(0), "Std.Array.Transpose2D");
  const std::size_t num_cols = first_row.Size();
  const std::size_t num_rows = grid.Size();

  for (std::size_t row_index = 1; row_index < num_rows; ++row_index) {
    if (const auto& row = validated_array_value(*grid.TryGet(row_index), "Std.Array.Transpose2D");
        row.Size() != num_cols) {
      throw std::invalid_argument(std::format("ArrayTranspose2D: ragged grid detected at row {}", row_index));
    }
  }

  Array out_grid;
  out_grid.Reserve(num_cols);
  for (std::size_t column_index = 0; column_index < num_cols; ++column_index) {
    Array new_row;
    new_row.Reserve(num_rows);
    for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
      const auto& row = validated_array_value(*grid.TryGet(row_index), "Std.Array.Transpose2D");
      new_row.PushBack(*row.TryGet(column_index));
    }
    out_grid.PushBack(Value{std::move(new_row)});
  }

  return Value{std::move(out_grid)};
}

// arg = [grid, row_start, row_end, col_start, col_end]
// Returns a new grid containing elements in [row_start, row_end) x [col_start, col_end).
[[nodiscard]] inline auto ArraySlice2D(Value arg) -> Value {
  const auto& grid = validated_grid_value(array_at(arg, 0), "Std.Array.Slice2D");
  const std::size_t row_start =
      uint64_index(array_at(arg, 1));
  const std::size_t row_end = uint64_index(array_at(arg, 2));
  const std::size_t col_start =
      uint64_index(array_at(arg, 3));
  const std::size_t col_end = uint64_index(array_at(arg, 4));

  if (row_start > row_end) {
    throw std::invalid_argument(std::format("ArraySlice2D: row_start {} > row_end {}", row_start, row_end));
  }
  if (col_start > col_end) {
    throw std::invalid_argument(std::format("ArraySlice2D: col_start {} > col_end {}", col_start, col_end));
  }
  if (row_end > grid.Size()) {
    throw std::out_of_range(std::format("ArraySlice2D: row_end {} exceeds grid size {}", row_end, grid.Size()));
  }

  Array out_grid;
  out_grid.Reserve(row_end - row_start);
  for (std::size_t row_index = row_start; row_index < row_end; ++row_index) {
    const auto& row = validated_array_value(*grid.TryGet(row_index), "Std.Array.Slice2D");
    if (col_end > row.Size()) {
      throw std::out_of_range(
          std::format("ArraySlice2D: col_end {} exceeds row {} size {}", col_end, row_index, row.Size()));
    }

    Array new_row;
    new_row.Reserve(col_end - col_start);
    for (std::size_t column_index = col_start; column_index < col_end; ++column_index) {
      new_row.PushBack(*row.TryGet(column_index));
    }
    out_grid.PushBack(Value{std::move(new_row)});
  }

  return Value{std::move(out_grid)};
}

// arg = [flat_array, rows, cols]
// Reshapes a flattened array into a grid of rows x cols in row-major order.
[[nodiscard]] inline auto ArrayReshape(Value arg) -> Value {
  const auto& flat = validated_array_value(array_at(arg, 0), "Std.Array.Reshape");
  const std::size_t num_rows = uint64_index(array_at(arg, 1));
  const std::size_t num_cols = uint64_index(array_at(arg, 2));

  if (num_cols != 0 && num_rows > (std::numeric_limits<std::size_t>::max() / num_cols)) {
    throw std::invalid_argument("ArrayReshape: rows * cols overflows size_t");
  }
  if (const std::size_t expected_size = num_rows * num_cols; flat.Size() != expected_size) {
    throw std::invalid_argument(std::format("ArrayReshape: flat_array has {} elements, but {}x{} requires {}",
                                            flat.Size(), num_rows, num_cols, expected_size));
  }

  Array out_grid;
  out_grid.Reserve(num_rows);
  for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
    Array new_row;
    new_row.Reserve(num_cols);
    for (std::size_t column_index = 0; column_index < num_cols; ++column_index) {
      const std::size_t flat_idx = row_index * num_cols + column_index;
      new_row.PushBack(*flat.TryGet(flat_idx));
    }
    out_grid.PushBack(Value{std::move(new_row)});
  }

  return Value{std::move(out_grid)};
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

  Array out;
  out.Reserve(shape->size());
  for (const std::size_t dim : *shape) {
    out.PushBack(make_uint(static_cast<UInt>(dim)));
  }
  return Value{std::move(out)};
}

// arg = value
[[nodiscard]] inline auto ArrayFlatten(Value arg) -> Value {
  Array out;
  flatten_into(arg, out);
  return Value{std::move(out)};
}

// arg = [value, indices]
[[nodiscard]] inline auto ArrayGetAtND(Value arg) -> Value {
  const Value* current = &array_at(arg, 0);
  const auto indices = uint64_indices(array_at(arg, 1));

  for (std::size_t depth = 0; depth < indices.size(); ++depth) {
    const auto& arr = as_array(*current);
    const std::size_t idx = indices.at(depth);
    if (idx >= arr.Size()) {
      throw std::out_of_range(
          std::format("ArrayGetAtND: index {} out of range at depth {} for size {}", idx, depth, arr.Size()));
    }
    current = &*arr.TryGet(idx);
  }
  return *current;
}

// arg = [value, indices, replacement]
[[nodiscard]] inline auto ArraySetAtND(Value arg) -> Value {
  const Value& value = array_at(arg, 0);
  const Value& replacement = array_at(arg, 2);

  const auto path =
      uint64_indices(array_at(arg, 1));
  return set_at_path(value, path, 0, replacement);
}

// arg = [flat_array, shape]
[[nodiscard]] inline auto ArrayReshapeND(Value arg) -> Value {
  const auto& flat = validated_array_value(array_at(arg, 0), "Std.Array.ReshapeND");
  const auto dims =
      uint64_indices(array_at(arg, 1));

  std::size_t total = 1;
  for (std::size_t shape_index = 0; shape_index < dims.size(); ++shape_index) {
    const std::size_t dim = dims.at(shape_index);
    if (dim != 0 && total > (std::numeric_limits<std::size_t>::max() / dim)) {
      throw std::invalid_argument("ArrayReshapeND: shape product overflows size_t");
    }
    total *= dim;
  }

  if (flat.Size() != total) {
    throw std::invalid_argument(
        std::format("ArrayReshapeND: flat array size {} does not match shape product {}", flat.Size(), total));
  }

  if (dims.empty()) {
    if (flat.Size() != 1) {
      throw std::invalid_argument("ArrayReshapeND: scalar reshape requires exactly 1 flat element");
    }
    return *flat.TryGet(0);
  }

  std::size_t cursor = 0;
  return reshape_from_flat(flat, dims, 0, cursor);
}

}  // namespace fleaux::runtime
