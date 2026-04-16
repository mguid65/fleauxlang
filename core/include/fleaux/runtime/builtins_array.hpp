#pragma once
// Multidimensional array builtins (SetAt2D, Fill, Transpose2D, Slice2D, Reshape).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Multidimensional array builtins

[[nodiscard]] inline auto checked_index(const Value& value, const char* name) -> std::size_t {
  const Int i = as_int_value(value);
  if (i < 0) { throw std::out_of_range(std::format("{} expects non-negative index, got {}", name, i)); }
  return static_cast<std::size_t>(i);
}

[[nodiscard]] inline auto array_shape(const Value& v) -> std::optional<std::vector<std::size_t>> {
  if (!v.HasArray()) { return std::vector<std::size_t>{}; }

  const auto& arr = as_array(v);
  std::vector<std::size_t> shape;
  shape.push_back(arr.Size());
  if (arr.Size() == 0) { return shape; }

  auto first_child_shape = array_shape(*arr.TryGet(0));
  if (!first_child_shape.has_value()) { return std::nullopt; }
  for (std::size_t i = 1; i < arr.Size(); ++i) {
    if (auto child_shape = array_shape(*arr.TryGet(i));
        !child_shape.has_value() || child_shape.value() != first_child_shape.value()) {
      return std::nullopt;
    }
  }

  shape.insert(shape.end(), first_child_shape->begin(), first_child_shape->end());
  return shape;
}

inline void flatten_into(const Value& v, Array& out) {
  if (!v.HasArray()) {
    out.PushBack(v);
    return;
  }
  const auto& arr = as_array(v);
  for (std::size_t i = 0; i < arr.Size(); ++i) { flatten_into(*arr.TryGet(i), out); }
}

[[nodiscard]] inline auto set_at_path(const Value& v, const std::vector<std::size_t>& path, const std::size_t depth,
                                      const Value& replacement) -> Value {
  if (depth == path.size()) { return replacement; }
  const auto& arr = as_array(v);
  const std::size_t idx = path.at(depth);
  if (idx >= arr.Size()) {
    throw std::out_of_range(
        std::format("Array.SetAtND: index {} out of range at depth {} for size {}", idx, depth, arr.Size()));
  }

  Array out;
  out.Reserve(arr.Size());
  for (std::size_t i = 0; i < arr.Size(); ++i) {
    if (i == idx) {
      out.PushBack(set_at_path(*arr.TryGet(i), path, depth + 1, replacement));
    } else {
      out.PushBack(*arr.TryGet(i));
    }
  }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto reshape_from_flat(const Array& flat, const std::vector<std::size_t>& dims, std::size_t depth,
                                            std::size_t& cursor) -> Value {
  if (depth == dims.size()) { return *flat.TryGet(cursor++); }

  Array out;
  out.Reserve(dims.at(depth));
  for (std::size_t i = 0; i < dims.at(depth); ++i) { out.PushBack(reshape_from_flat(flat, dims, depth + 1, cursor)); }
  return Value{std::move(out)};
}

struct ArrayGetAt {
  // arg = [array, index]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "ArrayGetAt");
    const auto& arr = as_array(*args.TryGet(0));
    const std::size_t idx = checked_index(*args.TryGet(1), "ArrayGetAt index");
    if (idx >= arr.Size()) {
      throw std::out_of_range(std::format("ArrayGetAt: index {} out of range for size {}", idx, arr.Size()));
    }
    return *arr.TryGet(idx);
  }
};

struct ArraySetAt {
  // arg = [array, index, value]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "ArraySetAt");
    const auto& arr = as_array(*args.TryGet(0));
    const std::size_t idx = checked_index(*args.TryGet(1), "ArraySetAt index");
    const Value& replacement = *args.TryGet(2);
    if (idx >= arr.Size()) {
      throw std::out_of_range(std::format("ArraySetAt: index {} out of range for size {}", idx, arr.Size()));
    }

    Array out;
    out.Reserve(arr.Size());
    for (std::size_t i = 0; i < arr.Size(); ++i) { out.PushBack(i == idx ? replacement : *arr.TryGet(i)); }
    return Value{std::move(out)};
  }
};

struct ArrayInsertAt {
  // arg = [array, index, value]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "ArrayInsertAt");
    const auto& arr = as_array(*args.TryGet(0));
    const std::size_t idx = checked_index(*args.TryGet(1), "ArrayInsertAt index");
    const Value& value = *args.TryGet(2);
    if (idx > arr.Size()) {
      throw std::out_of_range(std::format("ArrayInsertAt: index {} out of range for size {}", idx, arr.Size()));
    }

    Array out;
    out.Reserve(arr.Size() + 1);
    for (std::size_t i = 0; i < idx; ++i) { out.PushBack(*arr.TryGet(i)); }
    out.PushBack(value);
    for (std::size_t i = idx; i < arr.Size(); ++i) { out.PushBack(*arr.TryGet(i)); }
    return Value{std::move(out)};
  }
};

struct ArrayRemoveAt {
  // arg = [array, index]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "ArrayRemoveAt");
    const auto& arr = as_array(*args.TryGet(0));
    const std::size_t idx = checked_index(*args.TryGet(1), "ArrayRemoveAt index");
    if (idx >= arr.Size()) {
      throw std::out_of_range(std::format("ArrayRemoveAt: index {} out of range for size {}", idx, arr.Size()));
    }

    Array out;
    out.Reserve(arr.Size() - 1);
    for (std::size_t i = 0; i < arr.Size(); ++i) {
      if (i != idx) { out.PushBack(*arr.TryGet(i)); }
    }
    return Value{std::move(out)};
  }
};

struct ArraySlice {
  // arg = [array, start, stop], stop exclusive
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "ArraySlice");
    const auto& arr = as_array(*args.TryGet(0));
    const std::size_t start = checked_index(*args.TryGet(1), "ArraySlice start");
    const std::size_t stop = checked_index(*args.TryGet(2), "ArraySlice stop");
    if (start > stop) { throw std::invalid_argument(std::format("ArraySlice: start {} > stop {}", start, stop)); }
    if (stop > arr.Size()) {
      throw std::out_of_range(std::format("ArraySlice: stop {} out of range for size {}", stop, arr.Size()));
    }

    Array out;
    out.Reserve(stop - start);
    for (std::size_t i = start; i < stop; ++i) { out.PushBack(*arr.TryGet(i)); }
    return Value{std::move(out)};
  }
};

struct ArrayConcat {
  // arg = [lhs, rhs]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "ArrayConcat");
    const auto& lhs = as_array(*args.TryGet(0));
    const auto& rhs = as_array(*args.TryGet(1));

    Array out;
    out.Reserve(lhs.Size() + rhs.Size());
    for (std::size_t i = 0; i < lhs.Size(); ++i) { out.PushBack(*lhs.TryGet(i)); }
    for (std::size_t i = 0; i < rhs.Size(); ++i) { out.PushBack(*rhs.TryGet(i)); }
    return Value{std::move(out)};
  }
};

struct ArraySetAt2D {
  // arg = [grid, row, col, value]
  // grid is a tuple of tuples (rows x cols)
  // Returns a new grid with element at (row, col) replaced.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 4, "ArraySetAt2D");
    const auto& grid = as_array(*args.TryGet(0));
    const std::size_t row_idx = checked_index(*args.TryGet(1), "ArraySetAt2D row");
    const std::size_t col_idx = checked_index(*args.TryGet(2), "ArraySetAt2D col");
    const Value& new_val = *args.TryGet(3);

    if (row_idx >= grid.Size()) {
      throw std::out_of_range(std::format("ArraySetAt2D: row {} out of range for {} rows", row_idx, grid.Size()));
    }

    if (const auto& target_row = as_array(*grid.TryGet(row_idx)); col_idx >= target_row.Size()) {
      throw std::out_of_range(
          std::format("ArraySetAt2D: col {} out of range for row size {}", col_idx, target_row.Size()));
    }

    Array out_grid;
    out_grid.Reserve(grid.Size());
    for (std::size_t r = 0; r < grid.Size(); ++r) {
      const auto& row_val = *grid.TryGet(r);
      const auto& row = as_array(row_val);
      if (r != row_idx) {
        out_grid.PushBack(row_val);
        continue;
      }

      Array new_row;
      new_row.Reserve(row.Size());
      for (std::size_t c = 0; c < row.Size(); ++c) { new_row.PushBack(c == col_idx ? new_val : *row.TryGet(c)); }
      out_grid.PushBack(Value{std::move(new_row)});
    }

    return Value{std::move(out_grid)};
  }
};

struct ArrayFill {
  // arg = [tuple, start_index, length, value]
  // Returns a new tuple with elements [start_index, start_index+length) set to value.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 4, "ArrayFill");
    const auto& src = as_array(*args.TryGet(0));
    const std::size_t start_idx = checked_index(*args.TryGet(1), "ArrayFill start_index");
    const std::size_t fill_len = checked_index(*args.TryGet(2), "ArrayFill length");
    const Value& fill_val = *args.TryGet(3);

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
    for (std::size_t i = 0; i < src.Size(); ++i) {
      out.PushBack((i >= start_idx && i < end_idx) ? fill_val : *src.TryGet(i));
    }

    return Value{std::move(out)};
  }
};

struct ArrayTranspose2D {
  // arg = grid (tuple of tuples)
  // Returns a new grid where rows become columns and columns become rows.
  auto operator()(Value arg) const -> Value {
    const auto& grid = as_array(arg);
    if (grid.Size() == 0) { return Value{Array{}}; }

    const auto& first_row = as_array(*grid.TryGet(0));
    const std::size_t num_cols = first_row.Size();
    const std::size_t num_rows = grid.Size();

    for (std::size_t r = 1; r < num_rows; ++r) {
      if (const auto& row = as_array(*grid.TryGet(r)); row.Size() != num_cols) {
        throw std::invalid_argument(std::format("ArrayTranspose2D: ragged grid detected at row {}", r));
      }
    }

    Array out_grid;
    out_grid.Reserve(num_cols);
    for (std::size_t c = 0; c < num_cols; ++c) {
      Array new_row;
      new_row.Reserve(num_rows);
      for (std::size_t r = 0; r < num_rows; ++r) {
        const auto& row = as_array(*grid.TryGet(r));
        new_row.PushBack(*row.TryGet(c));
      }
      out_grid.PushBack(Value{std::move(new_row)});
    }

    return Value{std::move(out_grid)};
  }
};

struct ArraySlice2D {
  // arg = [grid, row_start, row_end, col_start, col_end]
  // Returns a new grid containing elements in [row_start, row_end) x [col_start, col_end).
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 5, "ArraySlice2D");
    const auto& grid = as_array(*args.TryGet(0));
    const std::size_t row_start = checked_index(*args.TryGet(1), "ArraySlice2D row_start");
    const std::size_t row_end = checked_index(*args.TryGet(2), "ArraySlice2D row_end");
    const std::size_t col_start = checked_index(*args.TryGet(3), "ArraySlice2D col_start");
    const std::size_t col_end = checked_index(*args.TryGet(4), "ArraySlice2D col_end");

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
    for (std::size_t r = row_start; r < row_end; ++r) {
      const auto& row = as_array(*grid.TryGet(r));
      if (col_end > row.Size()) {
        throw std::out_of_range(std::format("ArraySlice2D: col_end {} exceeds row {} size {}", col_end, r, row.Size()));
      }

      Array new_row;
      new_row.Reserve(col_end - col_start);
      for (std::size_t c = col_start; c < col_end; ++c) { new_row.PushBack(*row.TryGet(c)); }
      out_grid.PushBack(Value{std::move(new_row)});
    }

    return Value{std::move(out_grid)};
  }
};

struct ArrayReshape {
  // arg = [flat_array, rows, cols]
  // Reshapes a flattened array into a grid of rows x cols in row-major order.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "ArrayReshape");
    const auto& flat = as_array(*args.TryGet(0));
    const std::size_t num_rows = checked_index(*args.TryGet(1), "ArrayReshape rows");
    const std::size_t num_cols = checked_index(*args.TryGet(2), "ArrayReshape cols");

    if (num_cols != 0 && num_rows > (std::numeric_limits<std::size_t>::max() / num_cols)) {
      throw std::invalid_argument("ArrayReshape: rows * cols overflows size_t");
    }
    if (const std::size_t expected_size = num_rows * num_cols; flat.Size() != expected_size) {
      throw std::invalid_argument(std::format("ArrayReshape: flat_array has {} elements, but {}x{} requires {}",
                                              flat.Size(), num_rows, num_cols, expected_size));
    }

    Array out_grid;
    out_grid.Reserve(num_rows);
    for (std::size_t r = 0; r < num_rows; ++r) {
      Array new_row;
      new_row.Reserve(num_cols);
      for (std::size_t c = 0; c < num_cols; ++c) {
        const std::size_t flat_idx = r * num_cols + c;
        new_row.PushBack(*flat.TryGet(flat_idx));
      }
      out_grid.PushBack(Value{std::move(new_row)});
    }

    return Value{std::move(out_grid)};
  }
};

struct ArrayRank {
  // arg = value
  auto operator()(Value arg) const -> Value {
    const auto shape = array_shape(arg);
    if (!shape.has_value()) { throw std::invalid_argument("ArrayRank: value is not rectangular"); }
    return make_int(static_cast<Int>(shape->size()));
  }
};

struct ArrayShape {
  // arg = value
  auto operator()(Value arg) const -> Value {
    const auto shape = array_shape(arg);
    if (!shape.has_value()) { throw std::invalid_argument("ArrayShape: value is not rectangular"); }

    Array out;
    out.Reserve(shape->size());
    for (const std::size_t dim : *shape) { out.PushBack(make_int(static_cast<Int>(dim))); }
    return Value{std::move(out)};
  }
};

struct ArrayFlatten {
  // arg = value
  auto operator()(Value arg) const -> Value {
    Array out;
    flatten_into(arg, out);
    return Value{std::move(out)};
  }
};

struct ArrayGetAtND {
  // arg = [value, indices]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "ArrayGetAtND");
    const Value* current = &*args.TryGet(0);
    const auto& indices = as_array(*args.TryGet(1));

    for (std::size_t depth = 0; depth < indices.Size(); ++depth) {
      const auto& arr = as_array(*current);
      const std::size_t idx = checked_index(*indices.TryGet(depth), "ArrayGetAtND index");
      if (idx >= arr.Size()) {
        throw std::out_of_range(
            std::format("ArrayGetAtND: index {} out of range at depth {} for size {}", idx, depth, arr.Size()));
      }
      current = &*arr.TryGet(idx);
    }
    return *current;
  }
};

struct ArraySetAtND {
  // arg = [value, indices, replacement]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "ArraySetAtND");
    const Value& value = *args.TryGet(0);
    const auto& indices = as_array(*args.TryGet(1));
    const Value& replacement = *args.TryGet(2);

    std::vector<std::size_t> path;
    path.reserve(indices.Size());
    for (std::size_t i = 0; i < indices.Size(); ++i) {
      path.push_back(checked_index(*indices.TryGet(i), "ArraySetAtND index"));
    }
    return set_at_path(value, path, 0, replacement);
  }
};

struct ArrayReshapeND {
  // arg = [flat_array, shape]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "ArrayReshapeND");
    const auto& flat = as_array(*args.TryGet(0));
    const auto& shape_tuple = as_array(*args.TryGet(1));

    std::vector<std::size_t> dims;
    dims.reserve(shape_tuple.Size());
    std::size_t total = 1;
    for (std::size_t i = 0; i < shape_tuple.Size(); ++i) {
      const std::size_t dim = checked_index(*shape_tuple.TryGet(i), "ArrayReshapeND shape");
      dims.push_back(dim);
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
};

}  // namespace fleaux::runtime
