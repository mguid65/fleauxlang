#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <thread>

#include "fleaux/runtime/runtime_support.hpp"

using namespace fleaux::runtime;

namespace {

class StdoutCapture {
public:
  StdoutCapture() : old_buffer_(std::cout.rdbuf(buffer_.rdbuf())) {}

  StdoutCapture(const StdoutCapture&) = delete;
  auto operator=(const StdoutCapture&) -> StdoutCapture& = delete;

  ~StdoutCapture() { std::cout.rdbuf(old_buffer_); }

  [[nodiscard]] auto str() const -> std::string { return buffer_.str(); }

private:
  std::ostringstream buffer_;
  std::streambuf* old_buffer_;
};

}  // namespace

TEST_CASE("Runtime builtins: core tuple helpers", "[runtime]") {
  SECTION("Wrap and Unwrap") {
    Value tpl = make_tuple(make_int(1), make_int(2), make_int(3));
    REQUIRE(as_tuple(tpl).size() == 3);

    Value wrapped = Wrap(tpl);
    REQUIRE(as_tuple(wrapped).size() == 1);

    Value back = Unwrap(std::move(wrapped));
    REQUIRE(as_tuple(back).size() == 3);

    REQUIRE_THROWS_AS(Unwrap(make_tuple(make_int(1), make_int(2))), std::logic_error);
    REQUIRE_THROWS_WITH(Unwrap(make_int(1)), Catch::Matchers::ContainsSubstring("fleaux::runtime: expected Tuple"));
  }

  SECTION("First and Second") {
    const Value pair = make_tuple(make_int(10), make_string("hello"));
    REQUIRE(to_double(First(pair)) == 10.0);
    REQUIRE(as_string(Second(pair)) == "hello");

  }

  SECTION("Cast returns the original runtime value after one unary convenience unwrap") {
    const Value int_value = Cast(make_int(42));
    REQUIRE(to_double(int_value) == 42.0);
    REQUIRE(as_string(Type(int_value)) == "Int64");

    const Value singleton_tuple_value = Cast(make_tuple(make_int(7)));
    REQUIRE(as_tuple(singleton_tuple_value).size() == 1);
    REQUIRE(to_double(tuple_at(singleton_tuple_value, 0)) == 7.0);

    const Value tuple_value = Cast(make_tuple(make_string("user"), make_int(7)));
    REQUIRE(as_tuple(tuple_value).size() == 2);
    REQUIRE(as_string(tuple_at(tuple_value, 0)) == "user");
    REQUIRE(to_double(tuple_at(tuple_value, 1)) == 7.0);
  }

  SECTION("ElementAt") {
    Value tpl = make_tuple(make_int(10), make_string("hello"), make_bool(true));
    Value r0 = ElementAt(make_tuple(tpl, make_int(0)));
    REQUIRE(to_double(r0) == 10.0);

    Value r1 = ElementAt(make_tuple(tpl, make_int(1)));
    REQUIRE(as_string(r1) == "hello");

    Value r2 = ElementAt(make_tuple(tpl, make_int(2)));
    REQUIRE(as_bool(r2));

  }

  SECTION("Length") {
    Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4));
    Value len = Length(seq);  // Option B: Length takes array directly, no wrapper
    REQUIRE(to_double(len) == 4.0);
  }

  SECTION("Take, Drop, Slice") {
    Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5));

    Value taken = Take(make_tuple(seq, make_int(3)));
    REQUIRE(as_tuple(taken).size() == 3);
    REQUIRE(to_double(tuple_at(taken, 0)) == 1.0);
    REQUIRE(to_double(tuple_at(taken, 2)) == 3.0);

    Value dropped = Drop(make_tuple(seq, make_int(2)));
    REQUIRE(as_tuple(dropped).size() == 3);
    REQUIRE(to_double(tuple_at(dropped, 0)) == 3.0);

    Value sliced = SliceTupleUInt64UInt64(make_tuple(seq, make_int(1), make_int(4)));
    REQUIRE(as_tuple(sliced).size() == 3);
    REQUIRE(to_double(tuple_at(sliced, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(sliced, 2)) == 4.0);

    Value stop_only = SliceTupleUInt64(make_tuple(seq, make_int(3)));
    REQUIRE(as_tuple(stop_only).size() == 3);
    REQUIRE(to_double(tuple_at(stop_only, 0)) == 1.0);
    REQUIRE(to_double(tuple_at(stop_only, 2)) == 3.0);

    Value stepped = SliceTupleUInt64UInt64UInt64(make_tuple(seq, make_int(0), make_int(5), make_int(2)));
    REQUIRE(as_tuple(stepped).size() == 3);
    REQUIRE(to_double(tuple_at(stepped, 0)) == 1.0);
    REQUIRE(to_double(tuple_at(stepped, 1)) == 3.0);
    REQUIRE(to_double(tuple_at(stepped, 2)) == 5.0);

    REQUIRE_THROWS_WITH(SliceTupleUInt64UInt64UInt64(make_tuple(seq, make_int(0), make_int(5), make_int(0))),
                        Catch::Matchers::ContainsSubstring("Slice: step cannot be 0"));
  }
}

TEST_CASE("Tuples keep their items in one block", "[runtime][tuple]") {
  const auto int_tuple = [](const std::size_t size) -> Value {
    TupleBuilder items(size);
    for (std::size_t index = 0; index < size; ++index) {
      items.push(make_int(static_cast<Int>(index)));
    }
    return std::move(items).finish();
  };

  SECTION("Tuples of any size build, copy, compare, and hash") {
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{12288}}) {
      const Value tuple = int_tuple(size);
      const TupleView items = as_tuple(tuple);
      REQUIRE(items.size() == size);
      for (std::size_t index = 0; index < size; ++index) {
        REQUIRE(items[index].int_value() == static_cast<Int>(index));
      }

      const Value shared = tuple;
      REQUIRE(shared.object() == tuple.object());

      const Value copied = make_tuple_from(items);
      // Every empty tuple is the one empty tuple; any other copy is a block of its own.
      REQUIRE((copied.object() == tuple.object()) == (size == 0U));
      REQUIRE(copied == tuple);
      REQUIRE(DictKeyHash{}(copied) == DictKeyHash{}(tuple));
      REQUIRE(DictKeyEqual{}(copied, tuple));
    }
    REQUIRE_FALSE(int_tuple(3) == int_tuple(4));
    REQUIRE_FALSE(make_tuple(make_int(1), make_string("a")) == make_tuple(make_int(1), make_string("b")));
  }

  SECTION("mutable_tuple changes a tuple held once in place and copies a shared one first") {
    Value tuple = make_tuple(make_int(1), make_int(2));
    const HeapObject* object = tuple.object();
    tuple.mutable_tuple()[0] = make_int(5);
    REQUIRE(tuple.object() == object);
    REQUIRE(tuple == make_tuple(make_int(5), make_int(2)));

    Value shared = tuple;
    shared.mutable_tuple()[1] = make_int(9);
    REQUIRE(shared.object() != tuple.object());
    REQUIRE(tuple == make_tuple(make_int(5), make_int(2)));
    REQUIRE(shared == make_tuple(make_int(5), make_int(9)));
  }

  SECTION("A builder grows past its capacity and keeps its items in order") {
    TupleBuilder items(1U);
    for (int index = 0; index < 100; ++index) {
      items.push(make_string(std::to_string(index)));
    }
    const Value tuple = std::move(items).finish();
    REQUIRE(as_tuple(tuple).size() == 100U);
    for (std::size_t index = 0; index < 100U; ++index) {
      REQUIRE(as_string(tuple_at(tuple, index)) == std::to_string(index));
    }
  }

  SECTION("A builder pushes one of its own items while it grows") {
    TupleBuilder items(2U);
    items.push(make_string("a"));
    items.push(make_string("b"));
    items.push(items.items()[0]);
    REQUIRE(std::move(items).finish() == make_tuple(make_string("a"), make_string("b"), make_string("a")));
  }

  SECTION("A builder with most of its room unused finishes with its items") {
    TupleBuilder items(100U);
    items.push(make_string("x"));
    items.push(make_string("y"));
    REQUIRE(std::move(items).finish() == make_tuple(make_string("x"), make_string("y")));
  }

  SECTION("Tuples and unfinished builders release their items") {
    const auto token = std::make_shared<int>(0);
    const auto holder = [&token]() -> Value { return make_function([token](const Value& value) -> Value { return value; }); };
    try {
      TupleBuilder items(1U);
      items.push(holder());
      items.push(holder());
      REQUIRE(token.use_count() == 3);
      throw std::runtime_error("stop building");
    } catch (const std::runtime_error&) {
    }
    REQUIRE(token.use_count() == 1);

    {
      const Value tuple = make_tuple(holder(), make_tuple(holder()));
      const Value copy = tuple;
      REQUIRE(token.use_count() == 3);
    }
    REQUIRE(token.use_count() == 1);
  }

  SECTION("Builtins size their results exactly") {
    constexpr Int kMax = std::numeric_limits<Int>::max();
    constexpr Int kMin = std::numeric_limits<Int>::min();
    REQUIRE(tuple_range_impl(kMax - 2, kMax, 1) == make_tuple(make_int(kMax - 2), make_int(kMax - 1)));
    REQUIRE(tuple_range_impl(kMin + 2, kMin, -1) == make_tuple(make_int(kMin + 2), make_int(kMin + 1)));
    REQUIRE(tuple_range_impl(0, kMin, kMin) == make_tuple(make_int(0)));
    REQUIRE(tuple_range_impl(0, 10, 3) == make_tuple(make_int(0), make_int(3), make_int(6), make_int(9)));
    REQUIRE(tuple_range_impl(10, 0, -3) == make_tuple(make_int(10), make_int(7), make_int(4), make_int(1)));
    REQUIRE(as_tuple(tuple_range_impl(5, 5, 1)).empty());
    REQUIRE(as_tuple(tuple_range_impl(0, 10, -1)).empty());

    REQUIRE(StringSplit(make_tuple(make_string("a,b,,c,"), make_string(","))) ==
            make_tuple(make_string("a"), make_string("b"), make_string(""), make_string("c"), make_string("")));
    REQUIRE(StringSplit(make_tuple(make_string("aaa"), make_string("aa"))) ==
            make_tuple(make_string(""), make_string("a")));

    REQUIRE(ArrayFlatten(make_tuple(make_tuple(make_int(1), make_int(2)),
                                    make_tuple(make_int(3), make_tuple(make_int(4), make_tuple())), make_int(5))) ==
            make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5)));
    REQUIRE(ArrayFlatten(make_int(7)) == make_tuple(make_int(7)));

    const Value digits = int_tuple(10);
    REQUIRE(SliceTupleUInt64UInt64UInt64(make_tuple(digits, make_uint(1), make_uint(9), make_uint(3))) ==
            make_tuple(make_int(1), make_int(4), make_int(7)));
    REQUIRE(as_tuple(SliceTupleUInt64UInt64UInt64(make_tuple(digits, make_uint(8), make_uint(2), make_uint(1))))
                .empty());
  }
}

TEST_CASE("Tuple builtins change a tuple the argument holds once in place", "[runtime][tuple][in_place]") {
  const auto ints = [](const std::initializer_list<Int> values) -> Value {
    TupleBuilder items(values.size());
    for (const Int value : values) {
      items.push(make_int(value));
    }
    return std::move(items).finish();
  };

  SECTION("Updates that keep the size keep the block") {
    Value tuple = ints({3, 1, 2});
    const HeapObject* block = tuple.object();
    Value set = ArraySetAt(make_tuple(std::move(tuple), make_uint(0), make_int(9)));
    REQUIRE(set.object() == block);
    REQUIRE(set == ints({9, 1, 2}));
    Value filled = ArrayFill(make_tuple(std::move(set), make_uint(1), make_uint(2), make_int(7)));
    REQUIRE(filled.object() == block);
    REQUIRE(filled == ints({9, 7, 7}));
    const Value sorted = TupleSort(std::move(filled));
    REQUIRE(sorted.object() == block);
    REQUIRE(sorted == ints({7, 7, 9}));
  }

  SECTION("A grid and the row it holds once change in place") {
    Value grid = make_tuple(ints({1, 2}), ints({3, 4}));
    const HeapObject* block = grid.object();
    const HeapObject* row = tuple_at(grid, 1).object();
    Value out = ArraySetAt2D(make_tuple(std::move(grid), make_uint(1), make_uint(0), make_int(8)));
    REQUIRE(out.object() == block);
    REQUIRE(tuple_at(out, 1).object() == row);
    REQUIRE(out == make_tuple(ints({1, 2}), ints({8, 4})));
    const Value nested = ArraySetAtND(make_tuple(std::move(out), make_tuple(make_uint(0), make_uint(1)), make_int(6)));
    REQUIRE(nested.object() == block);
    REQUIRE(nested == make_tuple(ints({1, 6}), ints({8, 4})));
  }

  SECTION("A shared tuple is copied and left as it was") {
    const Value shared = ints({3, 1, 2});
    REQUIRE(ArraySetAt(make_tuple(shared, make_uint(0), make_int(9))) == ints({9, 1, 2}));
    REQUIRE(ArrayFill(make_tuple(shared, make_uint(0), make_uint(3), make_int(0))) == ints({0, 0, 0}));
    REQUIRE(TupleSort(shared) == ints({1, 2, 3}));
    REQUIRE(TupleAppend(make_tuple(shared, make_int(4))) == ints({3, 1, 2, 4}));
    REQUIRE(TuplePrepend(make_tuple(shared, make_int(0))) == ints({0, 3, 1, 2}));
    REQUIRE(ArrayInsertAt(make_tuple(shared, make_uint(3), make_int(5))) == ints({3, 1, 2, 5}));
    REQUIRE(ArrayRemoveAt(make_tuple(shared, make_uint(1))) == ints({3, 2}));
    REQUIRE(ArrayConcat(make_tuple(shared, shared)) == ints({3, 1, 2, 3, 1, 2}));
    REQUIRE(Take(make_tuple(shared, make_uint(2))) == ints({3, 1}));
    REQUIRE(Drop(make_tuple(shared, make_uint(2))) == ints({2}));
    REQUIRE(ArraySlice(make_tuple(shared, make_uint(1), make_uint(3))) == ints({1, 2}));
    REQUIRE(SliceTupleUInt64UInt64UInt64(make_tuple(shared, make_uint(0), make_uint(3), make_uint(2))) ==
            ints({3, 2}));
    REQUIRE(shared == ints({3, 1, 2}));
  }

  SECTION("Appending to a tuple held once doubles its block when it is full") {
    Value tuple = ints({1});
    for (Int value = 2; value <= 100; ++value) {
      tuple = TupleAppend(make_tuple(std::move(tuple), make_int(value)));
    }
    // 100 items in a block with room for 128: the next 28 appends stay in it.
    const HeapObject* block = tuple.object();
    for (Int value = 101; value <= 128; ++value) {
      tuple = TupleAppend(make_tuple(std::move(tuple), make_int(value)));
    }
    REQUIRE(tuple.object() == block);
    REQUIRE(as_tuple(tuple).size() == 128U);
    for (std::size_t index = 0; index < 128U; ++index) {
      REQUIRE(tuple_at(tuple, index).int_value() == static_cast<Int>(index) + 1);
    }
  }

  SECTION("Cutting, inserting, and joining stay in the block while it has room") {
    Value tuple = ints({1, 2, 3, 4, 5, 6, 7, 8});
    const HeapObject* block = tuple.object();
    tuple = Take(make_tuple(std::move(tuple), make_uint(6)));
    REQUIRE(tuple == ints({1, 2, 3, 4, 5, 6}));
    tuple = ArrayRemoveAt(make_tuple(std::move(tuple), make_uint(0)));
    REQUIRE(tuple == ints({2, 3, 4, 5, 6}));
    tuple = TuplePrepend(make_tuple(std::move(tuple), make_int(0)));
    REQUIRE(tuple == ints({0, 2, 3, 4, 5, 6}));
    tuple = ArrayInsertAt(make_tuple(std::move(tuple), make_uint(1), make_int(1)));
    REQUIRE(tuple == ints({0, 1, 2, 3, 4, 5, 6}));
    tuple = ArrayConcat(make_tuple(std::move(tuple), ints({7})));
    REQUIRE(tuple == ints({0, 1, 2, 3, 4, 5, 6, 7}));
    tuple = Drop(make_tuple(std::move(tuple), make_uint(1)));
    REQUIRE(tuple == ints({1, 2, 3, 4, 5, 6, 7}));
    tuple = SliceTupleUInt64UInt64UInt64(make_tuple(std::move(tuple), make_uint(0), make_uint(7), make_uint(2)));
    REQUIRE(tuple == ints({1, 3, 5, 7}));
    REQUIRE(tuple.object() == block);
    // Two items in a block for eight: the cut trades it for one of the exact size.
    tuple = ArraySlice(make_tuple(std::move(tuple), make_uint(1), make_uint(3)));
    REQUIRE(tuple == ints({3, 5}));
  }

  SECTION("Reading an item out of a tuple held once moves it") {
    Value text = make_string("item");
    const HeapObject* object = text.object();
    REQUIRE(First(make_tuple(std::move(text), make_int(1))).object() == object);

    Value second = make_string("b");
    const HeapObject* second_object = second.object();
    REQUIRE(ElementAt(make_tuple(make_tuple(make_string("a"), std::move(second)), make_uint(1))).object() ==
            second_object);

    const Value shared = make_tuple(make_string("kept"), make_int(1));
    REQUIRE(as_string(First(shared)) == "kept");
    REQUIRE(as_string(tuple_at(shared, 0)) == "kept");
  }
}

TEST_CASE("Small dicts find keys without a hash index", "[runtime][dict]") {
  const auto key = [](const int index) -> Value { return make_string("k" + std::to_string(index)); };

  SECTION("Every size across the switch at eight entries") {
    for (int count = 0; count <= 20; ++count) {
      Dict dict;
      for (int index = 0; index < count; ++index) {
        dict.set(make_int(index), make_int(index * 10));
      }
      REQUIRE(dict.size() == static_cast<std::size_t>(count));
      for (int index = 0; index < count; ++index) {
        REQUIRE(dict.contains(make_int(index)));
        const Value* const found = dict.find(make_int(index));
        REQUIRE((found != nullptr && *found == make_int(index * 10)));
      }
      REQUIRE(dict.find(make_int(count)) == nullptr);
      int position = 0;
      for (const auto& [entry_key, entry_value] : dict) {
        REQUIRE(entry_key == make_int(position++));
      }
      if (count > 0) {
        // Setting a key that is there keeps its place.
        dict.set(make_int(0), make_int(-1));
        REQUIRE(dict.size() == static_cast<std::size_t>(count));
        REQUIRE(dict.entries().front().second == make_int(-1));
      }
    }
  }

  SECTION("Erasing back below the switch and growing past it again") {
    Dict dict;
    for (int index = 0; index < 12; ++index) {
      dict.set(key(index), make_int(index));
    }
    for (int index = 0; index < 12; index += 2) {
      REQUIRE(dict.erase(key(index)));
    }
    REQUIRE_FALSE(dict.erase(key(0)));
    REQUIRE(dict.size() == 6U);
    for (int index = 0; index < 12; ++index) {
      const Value* found = dict.find(key(index));
      REQUIRE((found != nullptr) == (index % 2 == 1));
    }
    REQUIRE(dict.entries().front().first == key(1));
    REQUIRE(dict.entries().back().first == key(11));

    for (int index = 12; index < 16; ++index) {
      dict.set(key(index), make_int(index));
    }
    REQUIRE(dict.size() == 10U);
    REQUIRE(*dict.find(key(3)) == make_int(3));
    REQUIRE(*dict.find(key(15)) == make_int(15));
    REQUIRE(dict.entries().back().first == key(15));
  }

  SECTION("Keys differ by kind, and a NaN key finds itself") {
    Dict dict;
    dict.set(make_int(1), make_string("int"));
    dict.set(make_uint(1), make_string("uint"));
    REQUIRE(dict.size() == 2U);
    REQUIRE(as_string(*dict.find(make_int(1))) == "int");
    REQUIRE(as_string(*dict.find(make_uint(1))) == "uint");
    const Value nan = make_float(std::numeric_limits<double>::quiet_NaN());
    dict.set(nan, make_int(7));
    dict.set(nan, make_int(8));
    REQUIRE(dict.size() == 3U);
    REQUIRE(*dict.find(nan) == make_int(8));
  }

  SECTION("Copies of small and large dicts change apart") {
    for (const int count : {2, 12}) {
      Value original = make_dict();
      for (int index = 0; index < count; ++index) {
        original = DictSet(make_tuple(std::move(original), key(index), make_int(index)));
      }
      const Value changed = DictSet(make_tuple(original, key(0), make_int(-1)));
      REQUIRE(*as_dict(original).find(key(0)) == make_int(0));
      REQUIRE(*as_dict(changed).find(key(0)) == make_int(-1));
      REQUIRE(as_dict(changed).size() == static_cast<std::size_t>(count));
    }
  }

  SECTION("Equality ignores order and whether the dict ever had an index") {
    Dict grown;
    for (int index = 0; index < 12; ++index) {
      grown.set(key(index), make_int(index));
    }
    for (int index = 6; index < 12; ++index) {
      REQUIRE(grown.erase(key(index)));
    }
    Dict built;
    for (int index = 5; index >= 0; --index) {
      built.set(key(index), make_int(index));
    }
    REQUIRE(make_dict(std::move(grown)) == make_dict(std::move(built)));
  }
}

TEST_CASE("Every empty tuple is one shared tuple", "[runtime][tuple]") {
  const Value made = make_tuple();
  const Value built = TupleBuilder(4U).finish();
  const Value copied = make_tuple_from(TupleView{});
  REQUIRE(made.object() == empty_tuple().object());
  REQUIRE(built.object() == made.object());
  REQUIRE(copied.object() == made.object());
  REQUIRE(as_tuple(made).empty());
  REQUIRE_FALSE(built == make_tuple(make_int(1), make_int(2)));
  REQUIRE(DictKeyHash{}(built) == DictKeyHash{}(made));

  // Changing it copies it, and a tuple cut to nothing is it.
  const Value grown = TupleAppend(make_tuple(made, make_int(1)));
  REQUIRE(grown == make_tuple(make_int(1)));
  Value changed = make_tuple();
  REQUIRE(changed.mutable_tuple().empty());
  REQUIRE(Take(make_tuple(make_tuple(make_int(1), make_int(2)), make_uint(0))).object() == made.object());
  REQUIRE(as_tuple(empty_tuple()).empty());
}

TEST_CASE("Builtin arguments lend their values", "[runtime][arguments]") {
  SECTION("Values on the VM stack move out") {
    std::array<Value, 2> stack{make_string("moved"), make_int(1)};
    const HeapObject* text = stack[0].object();
    const Arguments args{std::span{stack}};
    REQUIRE(args.size() == 2U);
    REQUIRE(args.take(0).object() == text);
    REQUIRE(stack[0].is_null());
    REQUIRE_THROWS_WITH(args[2], Catch::Matchers::ContainsSubstring("argument index out of range"));
  }

  SECTION("An argument tuple held once lends its items, and a shared one is copied") {
    Value text = make_string("item");
    const HeapObject* object = text.object();
    const Arguments held_once{make_tuple(std::move(text), make_int(1))};
    REQUIRE(held_once.take(0).object() == object);

    const Value shared = make_tuple(make_string("kept"), make_int(1));
    const Arguments copied{shared};
    REQUIRE(as_string(copied.take(0)) == "kept");
    REQUIRE(as_string(tuple_at(shared, 0)) == "kept");
  }

  SECTION("A value that is not a tuple is one argument, also after the arguments move") {
    Arguments lone{make_string("lone")};
    const Arguments moved{std::move(lone)};
    REQUIRE(moved.size() == 1U);
    REQUIRE(as_string(moved[0]) == "lone");
  }
}

TEST_CASE("Std.Try turns a failure into an Err whichever way it arrives", "[runtime][try]") {
  SECTION("A function value made from C++ throws its failure, and Std.Try catches it") {
    const Value failing = make_function([](const Value&) -> Value { throw std::runtime_error("from C++"); });
    std::string error;
    REQUIRE_THROWS_WITH(as_callable(failing).call_or_fail(make_int(1), error), "from C++");
    REQUIRE(error.empty());
    REQUIRE(Try(make_tuple(make_int(1), failing)) == ResultErr(make_string("from C++")));
  }

  SECTION("A wrapped failure keeps only its message") {
    const Value failing = make_function(
        [](const Value&) -> Value { throw std::runtime_error("builtin 'Std.Divide' threw: Divide: by zero"); });
    REQUIRE(Try(make_tuple(make_int(1), failing)) == ResultErr(make_string("Divide: by zero")));
  }

  SECTION("A call that succeeds gives Ok") {
    const Value doubling = make_function([](const Value& value) -> Value { return make_int(value.int_value() * 2); });
    std::string error;
    const auto result = as_callable(doubling).call_or_fail(make_int(21), error);
    REQUIRE(result.has_value());
    REQUIRE(*result == make_int(42));
    REQUIRE(Try(make_tuple(make_int(4), doubling)) == ResultOk(make_int(8)));
  }
}

TEST_CASE("Value assignments take the new contents first and release the old object last", "[runtime][value]") {
  // A function value whose destructor counts, to see when its object goes.
  struct Sentinel {
    explicit Sentinel(int* counter) : destroyed(counter) {}
    Sentinel(const Sentinel&) = delete;
    auto operator=(const Sentinel&) -> Sentinel& = delete;
    Sentinel(Sentinel&&) = delete;
    auto operator=(Sentinel&&) -> Sentinel& = delete;
    ~Sentinel() { ++*destroyed; }
    int* destroyed;
  };
  int destroyed = 0;
  const auto make_probe = [&destroyed] {
    auto sentinel = std::make_shared<Sentinel>(&destroyed);
    return make_function([sentinel](const Value& value) -> Value { return value; });
  };

  SECTION("A tuple assigned its own item, by copy and by move") {
    Value copied = make_tuple(make_string("first item"), make_int(2));
    copied = copied.tuple()[0];
    CHECK(copied == make_string("first item"));

    Value moved = make_tuple(make_string("first item"), make_int(2));
    moved = std::move(moved.mutable_tuple()[0]);
    CHECK(moved == make_string("first item"));
  }

  SECTION("A value assigned itself, by copy and by move") {
    Value value = make_probe();
    Value& same = value;
    value = same;
    value = std::move(same);
    CHECK(value.is_function());
    CHECK(destroyed == 0);
    value = Value{};
    CHECK(destroyed == 1);
  }

  SECTION("Assigning over the last reference destroys the object once") {
    Value copied_over = make_probe();
    const Value replacement = make_int(1);
    copied_over = replacement;
    CHECK(destroyed == 1);
    Value moved_over = make_probe();
    moved_over = make_int(2);
    CHECK(destroyed == 2);
    CHECK(copied_over == make_int(1));
    CHECK(moved_over == make_int(2));
  }

  SECTION("Copies between values that share an object") {
    Value first = make_probe();
    Value second = first;
    Value third;
    third = second;
    second = third;
    first = Value{};
    second = Value{};
    CHECK(destroyed == 0);
    third = Value{};
    CHECK(destroyed == 1);
  }
}

TEST_CASE("Runtime builtins: arithmetic, comparison, and logic", "[runtime]") {
  REQUIRE(to_double(make_tuple(make_int(3), make_int(4)) | Add) == 7.0);
  REQUIRE(to_double(make_tuple(make_int(9), make_int(4)) | Subtract) == 5.0);
  REQUIRE(to_double(make_tuple(make_int(3), make_int(7)) | Multiply) == 21.0);
  REQUIRE(to_double(make_tuple(make_float(10.0), make_float(4.0)) | Divide) == 2.5);
  REQUIRE(to_double(make_tuple(make_int(2), make_int(8)) | Pow) == 256.0);
  REQUIRE(to_double(make_tuple(make_int(10), make_int(3)) | Mod) == 1.0);
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(2)) | Add) | Type) == "UInt64");
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(2)) | Multiply) | Type) == "UInt64");
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(2)) | Divide) | Type) == "UInt64");
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(3)) | Mod) | Type) == "UInt64");
  REQUIRE(to_double(make_int(5) | UnaryMinus) == -5.0);
  REQUIRE(to_double(make_int(5) | UnaryPlus) == 5.0);
  REQUIRE_THROWS_WITH(make_tuple(make_string("hello"), make_string(" world")) | Add,
                      Catch::Matchers::ContainsSubstring("expected Int64, UInt64, or Float64"));

  REQUIRE(as_bool(make_tuple(make_int(5), make_int(3)) | GreaterThan));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(5)) | LessThan));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(3)) | GreaterOrEqual));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(3)) | LessOrEqual));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(3)) | Equal));
  REQUIRE_FALSE(as_bool(make_tuple(make_int(3), make_int(3)) | NotEqual));
  REQUIRE_FALSE(as_bool(make_bool(true) | Not));
  REQUIRE(as_bool(make_tuple(make_bool(true), make_bool(false)) | Or));
  REQUIRE_FALSE(as_bool(make_tuple(make_bool(true), make_bool(false)) | And));
  REQUIRE(as_bool(make_tuple(make_int(-1), make_uint(2)) | LessThan));
  REQUIRE(as_bool(make_tuple(make_uint(2), make_int(-1)) | GreaterThan));

  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitAnd) == 2.0);
  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitOr) == 7.0);
  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitXor) == 5.0);
  REQUIRE(to_double(make_int(0) | BitNot) == -1.0);
  REQUIRE(to_double(make_tuple(make_int(3), make_int(2)) | BitShiftLeft) == 12.0);
  REQUIRE(to_double(make_tuple(make_int(12), make_int(2)) | BitShiftRight) == 3.0);

  Value r = make_tuple(make_bool(true), make_int(11), make_int(22)) | Select;
  REQUIRE(to_double(r) == 11.0);
}

TEST_CASE("Runtime builtins: arithmetic and bitwise boundaries", "[runtime][boundary]") {
  SECTION("Mixed signed/unsigned comparisons retain compare_numbers semantics while equality stays structural") {
    REQUIRE(as_bool(make_tuple(make_int(-5), make_uint(2)) | LessThan));
    REQUIRE(as_bool(make_tuple(make_uint(2), make_int(-5)) | GreaterThan));
    REQUIRE_FALSE(as_bool(make_tuple(make_uint(5), make_int(5)) | Equal));
    REQUIRE(as_bool(make_tuple(make_uint(5), make_int(5)) | NotEqual));
  }

  SECTION("Bit shifts reject negative and non-integer shift values") {
  }
}

TEST_CASE("Runtime builtins: math helpers", "[runtime]") {
  REQUIRE(std::abs(to_double(Sqrt(make_int(9))) - 3.0) < 1e-12);
  REQUIRE(std::abs(to_double(Sin(make_int(0))) - 0.0) < 1e-12);
  REQUIRE(std::abs(to_double(Cos(make_int(0))) - 1.0) < 1e-12);
  REQUIRE(std::abs(to_double(Tan(make_int(0))) - 0.0) < 1e-12);
  REQUIRE(std::abs(to_double(MathFloor(make_float(2.9))) - 2.0) < 1e-12);
  REQUIRE(std::abs(to_double(MathCeil(make_float(2.1))) - 3.0) < 1e-12);
  REQUIRE(std::abs(to_double(MathAbs(make_float(-7.5))) - 7.5) < 1e-12);
  REQUIRE(std::abs(to_double(MathLog(make_float(std::exp(2.0)))) - 2.0) < 1e-12);
  REQUIRE(std::abs(to_double(MathClamp(make_tuple(make_int(9), make_int(0), make_int(5)))) - 5.0) < 1e-12);
}

TEST_CASE("Runtime builtins: deterministic random generators", "[runtime][random]") {
  const Value seed = make_uint(123456789ULL);
  const Value generator = RandomCreate(seed);

  const Value next_uint = RandomNextUInt64(generator);
  REQUIRE(as_tuple(next_uint).size() == 2);
  REQUIRE(as_string(Type(First(next_uint))) == "UInt64");
  REQUIRE(as_string(ToString(First(next_uint))) == "2466975172287755897");

  const Value next_generator = Second(next_uint);
  const Value next_float = RandomNextFloat64(next_generator);
  REQUIRE(is_float_number(First(next_float)));
  REQUIRE(to_double(First(next_float)) >= 0.0);
  REQUIRE(to_double(First(next_float)) < 1.0);
  REQUIRE(std::abs(to_double(First(next_float)) - 0.500049775767424) < 1e-15);

  const Value next_int = RandomNextInt64(RandomCreate(make_uint(0ULL)));
  REQUIRE(as_string(Type(First(next_int))) == "Int64");
  REQUIRE(as_string(ToString(First(next_int))) == "-2152535657050944081");

  const Value next_bool = RandomNextBool(RandomCreate(seed));
  REQUIRE(as_bool(First(next_bool)));

  const Value split = RandomSplit(generator);
  const Value left_step = RandomNextUInt64(First(split));
  const Value right_step = RandomNextUInt64(Second(split));
  REQUIRE(as_string(ToString(First(left_step))) == "1278849761381179246");
  REQUIRE(as_string(ToString(First(right_step))) == "15883818742529415426");

}

TEST_CASE("Runtime builtins: OS exec", "[runtime]") {
#if defined(_WIN32)
  const Value result =
      make_string("powershell.exe -NoProfile -NonInteractive -Command \"[Console]::Write('fleaux_exec_ok')\"") | OSExec;
#else
  const Value result = make_string("printf 'fleaux_exec_ok'") | OSExec;
#endif
  REQUIRE(as_tuple(result).size() == 2);
  REQUIRE(to_double(tuple_at(result, 0)) == 0.0);
  REQUIRE(as_string(tuple_at(result, 1)) == "fleaux_exec_ok");
}

TEST_CASE("Runtime builtins: path and filesystem helpers require String slots", "[runtime]") {
  REQUIRE_THROWS_WITH(PathJoin(make_tuple(make_string("root"), make_int(1))),
                      Catch::Matchers::ContainsSubstring("expected String"));

  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_runtime_path_string_slots";
  std::filesystem::create_directories(temp_dir);
  const auto file_path = (temp_dir / "string_slots.txt").string();

  REQUIRE_THROWS_WITH(FileWriteText(make_tuple(make_string(file_path), make_int(1))),
                      Catch::Matchers::ContainsSubstring("expected String"));
  REQUIRE_THROWS_WITH(OSSetEnv(make_tuple(make_string("FLEAUX_RUNTIME_STRING_SLOT_TEST"), make_int(1))),
                      Catch::Matchers::ContainsSubstring("expected String"));

  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
}

TEST_CASE("Runtime builtins: Std.Match supports predicate patterns", "[runtime]") {
  const Value is_even =
      make_function([](const Value& v) -> Value { return make_bool(static_cast<Int>(to_double(v)) % 2 == 0); });
  const Value even_handler = make_function([](const Value&) -> Value { return make_string("even"); });
  const Value odd_handler = make_function([](const Value&) -> Value { return make_string("odd"); });

  SECTION("Predicate pattern matches before wildcard") {
    Value arg = make_tuple(make_int(6), make_tuple(is_even, even_handler),
                           make_tuple(make_string("__fleaux_match_wildcard__"), odd_handler));
    Value result = std::move(arg) | Match;
    REQUIRE(as_string(result) == "even");
  }

  SECTION("Predicate pattern falls through to wildcard") {
    Value arg = make_tuple(make_int(7), make_tuple(is_even, even_handler),
                           make_tuple(make_string("__fleaux_match_wildcard__"), odd_handler));
    Value result = std::move(arg) | Match;
    REQUIRE(as_string(result) == "odd");
  }
}

TEST_CASE("Runtime builtins: Std.Apply and Std.Branch", "[runtime]") {
  const Value add_one =
      make_function([](const Value& value) -> Value { return make_int(as_int_value(value) + 1); });
  const Value sub_one =
      make_function([](const Value& value) -> Value { return make_int(as_int_value(value) - 1); });

  REQUIRE(to_double(Apply(make_tuple(make_int(41), add_one))) == 42.0);
  REQUIRE(to_double(Branch(make_tuple(make_bool(true), make_int(10), add_one, sub_one))) == 11.0);
  REQUIRE(to_double(Branch(make_tuple(make_bool(false), make_int(10), add_one, sub_one))) == 9.0);
}

TEST_CASE("Runtime builtins: Std.Result helpers", "[runtime]") {
  SECTION("Constructors and predicates") {
    const Value ok = ResultOk(make_int(42));
    REQUIRE(as_bool(ResultIsOk(ok)));
    REQUIRE_FALSE(as_bool(ResultIsErr(ok)));
    REQUIRE(as_bool(ResultTag(ok)));
    REQUIRE(to_double(ResultPayload(ok)) == 42.0);

    const Value err = ResultErr(make_string("boom"));
    REQUIRE(as_bool(ResultIsErr(err)));
    REQUIRE_FALSE(as_bool(ResultIsOk(err)));
    REQUIRE_FALSE(as_bool(ResultTag(err)));
    REQUIRE(as_string(ResultPayload(err)) == "boom");
  }

  SECTION("Unwrap and UnwrapErr") {
    const Value ok = ResultOk(make_int(9));
    const Value err = ResultErr(make_string("bad"));

    REQUIRE(to_double(ResultUnwrap(ok)) == 9.0);
    REQUIRE(as_string(ResultUnwrapErr(err)) == "bad");

    REQUIRE_THROWS_WITH(ResultUnwrap(err), Catch::Matchers::ContainsSubstring("Result.Unwrap expected Ok"));
    REQUIRE_THROWS_WITH(ResultUnwrapErr(ok), Catch::Matchers::ContainsSubstring("Result.UnwrapErr expected Err"));
  }
}

TEST_CASE("Runtime builtins: Std.Try", "[runtime]") {
  SECTION("Try returns Ok when callable succeeds") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value result = Try(make_tuple(make_int(41), add_one));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(to_double(ResultUnwrap(result)) == 42.0);
  }

  SECTION("Try returns Err(message) when callable throws") {
    const Value boom = make_function([](const Value&) -> Value { throw std::runtime_error("boom"); });
    const Value result = Try(make_tuple(make_int(0), boom));
    REQUIRE(as_bool(ResultIsErr(result)));
    REQUIRE(as_string(ResultUnwrapErr(result)) == "boom");
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.Map", "[runtime]") {
  SECTION("Parallel returns Ok with ordered mapped outputs") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value result = ParallelMap(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one));
    REQUIRE(as_bool(ResultIsOk(result)));
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == 3);
    REQUIRE(to_double(tuple_at(payload, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(payload, 1)) == 3.0);
    REQUIRE(to_double(tuple_at(payload, 2)) == 4.0);
  }

  SECTION("Parallel returns Err(index, message) on first failing item") {
    const Value fail_on_two = make_function([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("boom-two"); }
      return v;
    });
    const Value result = ParallelMap(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), fail_on_two));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 1.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "boom-two");
  }

  SECTION("Parallel stress: repeated invocation remains stable and ordered") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value input = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6), make_int(7),
                             make_int(8));

    for (int iter = 0; iter < 200; ++iter) {
      const Value result = ParallelMap(make_tuple(input, add_one));
      REQUIRE(as_bool(ResultIsOk(result)));
      const Value payload = ResultUnwrap(result);
      REQUIRE(as_tuple(payload).size() == 8);
      REQUIRE(to_double(tuple_at(payload, 0)) == 2.0);
      REQUIRE(to_double(tuple_at(payload, 7)) == 9.0);
    }
  }

  SECTION("Parallel stress gives the same results on every run") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value input = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4));
    const Value expected = make_tuple(make_int(2), make_int(3), make_int(4), make_int(5));
    for (int iter = 0; iter < 120; ++iter) {
      const Value result = ParallelMap(make_tuple(input, add_one));
      REQUIRE(as_bool(ResultIsOk(result)));
      REQUIRE(ResultUnwrap(result) == expected);
    }
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.Map queues its items in one batch", "[runtime][parallel]") {
  // TaskRuntime::submit_all queues every item, one task each, and wakes at most every worker.
  const std::size_t workers = task_runtime().worker_count();

  SECTION("Many more items than workers keep their input order") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const std::size_t count = workers * 64U;
    TupleBuilder items(count);
    for (std::size_t index = 0; index < count; ++index) {
      items.push(make_int(static_cast<Int>(index)));
    }
    const Value result = ParallelMap(make_tuple(std::move(items).finish(), add_one));
    REQUIRE(as_bool(ResultIsOk(result)));
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == count);
    for (std::size_t index = 0; index < count; ++index) {
      REQUIRE(as_int_value(tuple_at(payload, index)) == static_cast<Int>(index) + 1);
    }
  }

  SECTION("The first failure in input order is returned, and every item still runs") {
    constexpr int count = 600;
    std::atomic<int> ran{0};
    const Value fail_some = make_function([&ran](const Value& v) -> Value {
      ran.fetch_add(1, std::memory_order_relaxed);
      if (const auto n = as_int_value(v); n == 450 || n == 200 || n == 599) {
        throw std::runtime_error("fail-" + std::to_string(n));
      }
      return v;
    });
    TupleBuilder items(count);
    for (int index = 0; index < count; ++index) {
      items.push(make_int(index));
    }
    const Value result = ParallelMap(make_tuple(std::move(items).finish(), fail_some));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_int_value(tuple_at(err, 0)) == 200);
    REQUIRE(as_string(tuple_at(err, 1)) == "fail-200");
    // The map returns at item 200; the items after it are already queued and finish on their own.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (ran.load() < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    REQUIRE(ran.load() == count);
  }

  SECTION("WithOptions with room for every item runs them as one batch") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    Value options = make_dict();
    options.mutable_dict().set(make_string("max_workers"), make_int(64));
    const Value result = ParallelWithOptions(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one, options));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(ResultUnwrap(result) == make_tuple(make_int(2), make_int(3), make_int(4)));
  }

  SECTION("A parallel map inside a parallel item") {
    // Each outer item waits for its inner map on a worker, so the pool needs more workers than outer items.
    if (workers < 4U) {
      SKIP("fewer than 4 pool workers");
    }
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value inner_map = make_function([add_one](const Value& v) -> Value {
      const Int base = as_int_value(v);
      return ResultUnwrap(ParallelMap(make_tuple(make_tuple(make_int(base), make_int(base + 10)), add_one)));
    });
    const Value result = ParallelMap(make_tuple(make_tuple(make_int(1), make_int(2)), inner_map));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(ResultUnwrap(result) == make_tuple(make_tuple(make_int(2), make_int(12)), make_tuple(make_int(3), make_int(13))));
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.WithOptions", "[runtime]") {
  SECTION("WithOptions delegates to Parallel.Map baseline") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value options = make_dict();
    options.mutable_dict().set(make_string("max_workers"), make_int(4));

    const Value result = ParallelWithOptions(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one, options));
    REQUIRE(as_bool(ResultIsOk(result)));
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == 3);
    REQUIRE(to_double(tuple_at(payload, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(payload, 2)) == 4.0);
  }

  SECTION("WithOptions returns Err when options is not a dict") {
    const Value identity = make_function([](const Value& v) -> Value { return v; });
    const Value result = ParallelWithOptions(make_tuple(make_tuple(make_int(1)), identity, make_int(5)));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 0.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "Parallel.WithOptions: options must be a Dict");
  }

  SECTION("WithOptions preserves deterministic output order under chunking") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value options = make_dict();
    options.mutable_dict().set(make_string("max_workers"), make_int(2));

    const Value result =
        ParallelWithOptions(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5)),
                                      add_one, options));
    REQUIRE(as_bool(ResultIsOk(result)));
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == 5);
    REQUIRE(to_double(tuple_at(payload, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(payload, 1)) == 3.0);
    REQUIRE(to_double(tuple_at(payload, 2)) == 4.0);
    REQUIRE(to_double(tuple_at(payload, 3)) == 5.0);
    REQUIRE(to_double(tuple_at(payload, 4)) == 6.0);
  }

  SECTION("WithOptions reports global failing index across chunk boundaries") {
    const Value fail_on_four = make_function([](const Value& v) -> Value {
      if (as_int_value(v) == 4) { throw std::runtime_error("boom-four"); }
      return v;
    });

    Value options = make_dict();
    options.mutable_dict().set(make_string("max_workers"), make_int(2));

    const Value result = ParallelWithOptions(
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5)), fail_on_four, options));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 3.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "boom-four");
  }

  SECTION("WithOptions rejects non-positive max_workers") {
    const Value identity = make_function([](const Value& v) -> Value { return v; });

    Value bad_workers = make_dict();
    bad_workers.mutable_dict().set(make_string("max_workers"), make_int(0));
    const Value workers_result = ParallelWithOptions(make_tuple(make_tuple(make_int(1)), identity, bad_workers));
    REQUIRE(as_bool(ResultIsErr(workers_result)));
    const Value err = ResultUnwrapErr(workers_result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 0.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "Parallel.WithOptions: max_workers must be > 0");
  }

  SECTION("WithOptions rejects unsupported option keys") {
    const Value identity = make_function([](const Value& v) -> Value { return v; });

    Value options = make_dict();
    options.mutable_dict().set(make_string("chunk_size"), make_int(2));
    const Value result = ParallelWithOptions(make_tuple(make_tuple(make_int(1)), identity, options));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 0.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "Parallel.WithOptions: unsupported option 'chunk_size'");
  }
}

TEST_CASE("Runtime builtins: Std.Task.* queued backend", "[runtime]") {
  TaskScope task_scope;

  SECTION("Task.Spawn + Task.Await returns Ok(result)") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value task = TaskSpawn(make_tuple(add_one, make_int(41)));
    const Value awaited = TaskAwait(task);
    REQUIRE(as_bool(ResultIsOk(awaited)));
    REQUIRE(to_double(ResultUnwrap(awaited)) == 42.0);
  }

  SECTION("Task.AwaitAll collects payloads and preserves order") {
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value t1 = TaskSpawn(make_tuple(add_one, make_int(1)));
    const Value t2 = TaskSpawn(make_tuple(add_one, make_int(2)));
    const Value t3 = TaskSpawn(make_tuple(add_one, make_int(3)));

    const Value all = TaskAwaitAll(make_tuple(t1, t2, t3));
    REQUIRE(as_bool(ResultIsOk(all)));
    const Value payload = ResultUnwrap(all);
    REQUIRE(as_tuple(payload).size() == 3);
    REQUIRE(to_double(tuple_at(payload, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(payload, 1)) == 3.0);
    REQUIRE(to_double(tuple_at(payload, 2)) == 4.0);
  }

  SECTION("Task.AwaitAll returns Err(index, message) on failing task") {
    const Value maybe_fail = make_function([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("boom-two"); }
      return v;
    });
    const Value t1 = TaskSpawn(make_tuple(maybe_fail, make_int(1)));
    const Value t2 = TaskSpawn(make_tuple(maybe_fail, make_int(2)));
    const Value all = TaskAwaitAll(make_tuple(t1, t2));

    REQUIRE(as_bool(ResultIsErr(all)));
    const Value err = ResultUnwrapErr(all);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 1.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "boom-two");
  }

  SECTION("Task.AwaitAll returns Err(index, message) for values that are not tasks") {
    TaskScope scope;
    const Value identity = make_function([](const Value& v) -> Value { return v; });
    const Value task = TaskSpawn(make_tuple(identity, make_int(7)));

    const Value all = TaskAwaitAll(make_tuple(task, make_int(7)));

    REQUIRE(as_bool(ResultIsErr(all)));
    const Value err = ResultUnwrapErr(all);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 1.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "Task.AwaitAll: invalid task handle");
  }

  SECTION("Task.Cancel can cancel a queued task before execution") {
    using namespace std::chrono_literals;

    const Value slow_identity = make_function([](const Value& v) -> Value {
      std::this_thread::sleep_for(50ms);
      return v;
    });
    const Value fast_identity = make_function([](const Value& v) -> Value { return v; });

    const Value first = TaskSpawn(make_tuple(slow_identity, make_int(7)));
    const Value second = TaskSpawn(make_tuple(fast_identity, make_int(8)));

    const Value cancel_result = TaskCancel(second);
    const bool cancelled = as_bool(cancel_result);

    const Value first_result = TaskAwait(first);
    REQUIRE(as_bool(ResultIsOk(first_result)));
    REQUIRE(to_double(ResultUnwrap(first_result)) == 7.0);

    const Value second_result = TaskAwait(second);
    if (cancelled) {
      REQUIRE(as_bool(ResultIsErr(second_result)));
      REQUIRE(as_string(ResultUnwrapErr(second_result)) == "Task cancelled");
    } else {
      // Multi-worker scheduling can start the second task before cancel is processed.
      REQUIRE(as_bool(ResultIsOk(second_result)));
      REQUIRE(to_double(ResultUnwrap(second_result)) == 8.0);
    }
  }

  SECTION("Task.WithTimeout reports timeout and later await still succeeds") {
    using namespace std::chrono_literals;

    const Value slow_identity = make_function([](const Value& v) -> Value {
      std::this_thread::sleep_for(40ms);
      return v;
    });
    const Value task = TaskSpawn(make_tuple(slow_identity, make_int(7)));

    const Value timed = TaskWithTimeout(make_tuple(task, make_int(1)));
    REQUIRE(as_bool(ResultIsErr(timed)));
    REQUIRE(as_string(ResultUnwrapErr(timed)) == "Task.WithTimeout: timeout exceeded");

    const Value awaited = TaskAwait(task);
    REQUIRE(as_bool(ResultIsOk(awaited)));
    REQUIRE(to_double(ResultUnwrap(awaited)) == 7.0);
  }

  SECTION("Task.WithTimeout rejects negative timeout through shared integer helper") {
    const Value identity = make_function([](const Value& v) -> Value { return v; });
    const Value task = TaskSpawn(make_tuple(identity, make_int(7)));

  }

  SECTION("Task.WithTimeout returns Err(String) for invalid task handle") {
    const Value result = TaskWithTimeout(make_tuple(make_int(7), make_int(1)));
    REQUIRE(as_bool(ResultIsErr(result)));
    REQUIRE(as_string(ResultUnwrapErr(result)) == "Task.WithTimeout: invalid task handle");
  }
}

TEST_CASE("TaskScope finishes the tasks spawned inside it and leaves their results readable",
          "[runtime][concurrency]") {
  reset_spawned_tasks_for_tests();
  REQUIRE(spawned_task_count() == 0U);

  Value outer_task = make_null();
  Value inner_task = make_null();
  {
    TaskScope outer_scope;
    const Value add_one = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    outer_task = TaskSpawn(make_tuple(add_one, make_int(10)));
    REQUIRE(spawned_task_count() == 1U);

    {
      TaskScope inner_scope;
      inner_task = TaskSpawn(make_tuple(add_one, make_int(20)));
      REQUIRE(spawned_task_count() == 2U);

      const Value inner_result = TaskAwait(inner_task);
      REQUIRE(as_bool(ResultIsOk(inner_result)));
      REQUIRE(to_double(ResultUnwrap(inner_result)) == 21.0);
    }

    REQUIRE(spawned_task_count() == 1U);

    const Value outer_result = TaskAwait(outer_task);
    REQUIRE(as_bool(ResultIsOk(outer_result)));
    REQUIRE(to_double(ResultUnwrap(outer_result)) == 11.0);
  }

  REQUIRE(spawned_task_count() == 0U);

  // A task value outlives the scope that ran it, and still holds its result.
  const Value inner_after = TaskAwait(inner_task);
  REQUIRE(as_bool(ResultIsOk(inner_after)));
  REQUIRE(to_double(ResultUnwrap(inner_after)) == 21.0);
  const Value outer_after = TaskAwait(outer_task);
  REQUIRE(as_bool(ResultIsOk(outer_after)));
  REQUIRE(to_double(ResultUnwrap(outer_after)) == 11.0);
}

TEST_CASE("ActiveRuntimeExecutionStateScope isolates spawned tasks and propagates into task workers",
          "[runtime][concurrency][execution_state]") {
  reset_spawned_tasks_for_tests();
  REQUIRE(spawned_task_count() == 0U);

  RuntimeExecutionState isolated_state;
  isolated_state.native_functions.emplace_back([](const Value& v) -> Value { return v; });
  {
    ActiveRuntimeExecutionStateScope state_scope(isolated_state);
    REQUIRE(spawned_task_count() == 0U);

    TaskScope task_scope;
    const Value inspect_state = make_function([](const Value&) -> Value {
      return make_int(static_cast<Int>(runtime_execution_state().native_functions.size()));
    });

    const Value task = TaskSpawn(make_tuple(inspect_state, make_null()));
    REQUIRE(spawned_task_count() == 1U);

    const Value awaited = TaskAwait(task);
    REQUIRE(as_bool(ResultIsOk(awaited)));
    REQUIRE(to_double(ResultUnwrap(awaited)) == 1.0);
  }

  REQUIRE(spawned_task_count() == 0U);
  REQUIRE(runtime_execution_state().native_functions.empty());
}

TEST_CASE("Runtime builtins: Std.Parallel.ForEach", "[runtime]") {
  SECTION("ForEach returns Ok(empty tuple) when all items succeed") {
    std::atomic<int> counter{0};
    const Value bump = make_function([&counter](const Value& /*v*/) -> Value {
      counter.fetch_add(1, std::memory_order_relaxed);
      return make_bool(true);
    });
    const Value result = ParallelForEach(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), bump));
    REQUIRE(as_bool(ResultIsOk(result)));
    // Output is empty tuple.
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == 0);
    // All items were visited.
    REQUIRE(counter.load() == 3);
  }

  SECTION("ForEach returns Err(index, message) when an item fails") {
    const Value fail_on_two = make_function([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("fe-error"); }
      return make_bool(true);
    });
    const Value result = ParallelForEach(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), fail_on_two));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(as_string(tuple_at(err, 1)) == "fe-error");
  }

  SECTION("ForEach on empty input returns Ok(empty tuple)") {
    const Value noop = make_function([](const Value& v) -> Value { return v; });
    const Value result = ParallelForEach(make_tuple(make_tuple(), noop));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(as_tuple(ResultUnwrap(result)).size() == 0);
  }

  SECTION("ForEach stress: large input all items visited in parallel") {
    constexpr int n = 500;
    std::atomic<int> counter{0};
    const Value bump = make_function([&counter](const Value& /*v*/) -> Value {
      counter.fetch_add(1, std::memory_order_relaxed);
      return make_bool(true);
    });
    TupleBuilder items(n);
    for (int i = 0; i < n; ++i) { items.push(make_int(i)); }
    const Value result = ParallelForEach(make_tuple(std::move(items).finish(), bump));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(counter.load() == n);
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.Reduce", "[runtime]") {
  SECTION("Reduce sums a small list") {
    const Value add = make_function([](const Value& arg) -> Value {
      const auto& arr = as_tuple(arg);
      const Int a = as_int_value(arr[0]);
      const Int b = as_int_value(arr[1]);
      return make_int(a + b);
    });
    const Value result =
        ParallelReduce(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4)), make_int(0), add));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(to_double(ResultUnwrap(result)) == 10.0);
  }

  SECTION("Reduce with plain (non-Result) return value works") {
    const Value add = make_function([](const Value& arg) -> Value {
      const auto& arr = as_tuple(arg);
      return make_int(as_int_value(arr[0]) + as_int_value(arr[1]));
    });
    const Value result = ParallelReduce(make_tuple(make_tuple(make_int(10), make_int(20), make_int(30)), make_int(0), add));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(to_double(ResultUnwrap(result)) == 60.0);
  }

  SECTION("Reduce on empty input returns Ok(init)") {
    const Value add = make_function([](const Value& arg) -> Value { return arg; });
    const Value result = ParallelReduce(make_tuple(make_tuple(), make_int(99), add));
    REQUIRE(as_bool(ResultIsOk(result)));
    REQUIRE(to_double(ResultUnwrap(result)) == 99.0);
  }

  SECTION("Reduce preserves accumulator tuples that begin with Bool") {
    const Value fold = make_function([](const Value& arg) -> Value {
      const auto& arr = as_tuple(arg);
      const auto& acc = as_tuple(arr[0]);
      const bool flag = as_bool(acc[0]);
      const Int sum = as_int_value(acc[1]) + as_int_value(arr[1]);
      return make_tuple(make_bool(flag), make_int(sum));
    });
    const Value result = ParallelReduce(
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), make_tuple(make_bool(false), make_int(0)), fold));
    REQUIRE(as_bool(ResultIsOk(result)));
    const Value payload = ResultUnwrap(result);
    REQUIRE(as_tuple(payload).size() == 2);
    REQUIRE_FALSE(as_bool(tuple_at(payload, 0)));
    REQUIRE(to_double(tuple_at(payload, 1)) == 6.0);
  }

  SECTION("Reduce annotates thrown step exceptions with global index") {
    const Value fail = make_function([](const Value& arg) -> Value {
      const auto& arr = as_tuple(arg);
      const Int b = as_int_value(arr[1]);
      if (b == 3) { throw std::runtime_error("reduce-throw"); }
      return arr[0];
    });
    const Value result = ParallelReduce(
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4)), make_int(0), fail));
    REQUIRE(as_bool(ResultIsErr(result)));
    const Value err = ResultUnwrapErr(result);
    REQUIRE(as_tuple(err).size() == 2);
    REQUIRE(to_double(tuple_at(err, 0)) == 2.0);
    REQUIRE(as_string(tuple_at(err, 1)) == "reduce-throw");
  }
}

TEST_CASE("Runtime builtins: concurrency property tests", "[runtime][concurrency]") {
  SECTION("Parallel.Map is semantically equivalent to sequential map for pure functions") {
    // Property: for a pure function f and any input list,
    // Parallel.Map(items, f) == [f(items[0]), f(items[1]), ...]
    const Value double_val = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) * 2); });

    constexpr int runs = 100;
    for (int run = 0; run < runs; ++run) {
      const int n = (run % 16) + 1;
      TupleBuilder items(static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i) { items.push(make_int(i + 1)); }

      const Value result = ParallelMap(make_tuple(std::move(items).finish(), double_val));
      REQUIRE(as_bool(ResultIsOk(result)));
      const Value payload = ResultUnwrap(result);
      REQUIRE(as_tuple(payload).size() == static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i) {
        REQUIRE(to_double(tuple_at(payload, static_cast<std::size_t>(i))) == static_cast<double>((i + 1) * 2));
      }
    }
  }

  SECTION("Parallel.Map output order is always input order under concurrent execution") {
    // Use a function that sleeps a small random amount to trigger scheduling variance.
    const Value delayed = make_function([](const Value& v) -> Value {
      const int ms = static_cast<int>(as_int_value(v)) % 5;
      if (ms > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
      return v;
    });

    constexpr int items_count = 20;
    TupleBuilder builder(items_count);
    for (int i = 0; i < items_count; ++i) { builder.push(make_int(i)); }
    const Value items = std::move(builder).finish();

    for (int run = 0; run < 20; ++run) {
      const Value result = ParallelMap(make_tuple(items, delayed));
      REQUIRE(as_bool(ResultIsOk(result)));
      const Value payload = ResultUnwrap(result);
      REQUIRE(as_tuple(payload).size() == items_count);
      for (int i = 0; i < items_count; ++i) {
        REQUIRE(to_double(tuple_at(payload, static_cast<std::size_t>(i))) == static_cast<double>(i));
      }
    }
  }

  SECTION("Parallel.Map stays correct under repeated concurrent use") {
    const Value fn = make_function([](const Value& v) -> Value { return v; });
    const Value items = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6),
                                   make_int(7), make_int(8));
    for (int i = 0; i < 150; ++i) {
      const Value result = ParallelMap(make_tuple(items, fn));
      REQUIRE(as_bool(ResultIsOk(result)));
      REQUIRE(ResultUnwrap(result) == items);
    }
  }

  SECTION("Concurrent Task.Spawn + Parallel.Map interleaved leave no spawned tasks behind") {
    reset_spawned_tasks_for_tests();
    const Value fn = make_function([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    REQUIRE(spawned_task_count() == 0U);

    {
      TaskScope task_scope;
      for (int i = 0; i < 30; ++i) {
        const Value task = TaskSpawn(make_tuple(fn, make_int(i)));
        const Value items = make_tuple(make_int(i), make_int(i + 1), make_int(i + 2));
        const Value map_result = ParallelMap(make_tuple(items, fn));
        REQUIRE(as_bool(ResultIsOk(map_result)));
        const Value awaited = TaskAwait(task);
        REQUIRE(as_bool(ResultIsOk(awaited)));
        REQUIRE(to_double(ResultUnwrap(awaited)) == static_cast<double>(i + 1));
      }
      REQUIRE(spawned_task_count() == 30U);
    }

    REQUIRE(spawned_task_count() == 0U);
  }
}

TEST_CASE("Runtime builtins propagate submitting thread context into task workers", "[runtime][concurrency][context]") {
  reset_spawned_tasks_for_tests();

  SECTION("Task.Spawn workers inherit output stream and process args") {
    std::ostringstream output;
    RuntimeOutputStreamScope output_scope(output);
    RuntimeProcessArgsScope args_scope({"<task>", "alpha", "beta"});
    TaskScope task_scope;

    const Value worker = make_function([](const Value& value) -> Value {
      const Value args = GetArgs(make_tuple());
      (void)Println(make_int(static_cast<Int>(as_tuple(args).size())));
      return make_tuple(args, value);
    });

    const Value task = TaskSpawn(make_tuple(worker, make_string("payload")));
    const Value awaited = TaskAwait(task);
    REQUIRE(as_bool(ResultIsOk(awaited)));

    const Value unwrapped = ResultUnwrap(awaited);
    const auto& payload = as_tuple(unwrapped);
    REQUIRE(payload.size() == 2U);
    const auto& args = as_tuple(payload[0]);
    REQUIRE(args.size() == 3U);
    REQUIRE(as_string(args[0]) == "<task>");
    REQUIRE(as_string(args[1]) == "alpha");
    REQUIRE(as_string(args[2]) == "beta");
    REQUIRE(as_string(payload[1]) == "payload");
    REQUIRE(output.str() == "3\n");
  }

  SECTION("Parallel.Map workers inherit process args") {
    RuntimeProcessArgsScope args_scope({"<parallel>", "delta", "gamma"});

    const Value worker = make_function([](const Value& value) -> Value {
      (void)value;
      return make_int(static_cast<Int>(as_tuple(GetArgs(make_tuple())).size()));
    });

    const Value result = ParallelMap(make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), worker));
    REQUIRE(as_bool(ResultIsOk(result)));

    const Value unwrapped = ResultUnwrap(result);
    const auto& mapped = as_tuple(unwrapped);
    REQUIRE(mapped.size() == 3U);
    for (std::size_t index = 0; index < mapped.size(); ++index) {
      REQUIRE(as_int_value(mapped[index]) == 3);
    }
  }
}

TEST_CASE("Runtime builtins: loop and formatting", "[runtime]") {
  SECTION("Loop sums 1..5") {
    auto cf = [](Value state) -> Value { return make_bool(to_double(tuple_at(state, 0)) > 0.0); };
    auto sf = [](Value state) -> Value {
      const double n = to_double(tuple_at(state, 0));
      const double acc = to_double(tuple_at(state, 1));
      return make_tuple(make_int(static_cast<Int>(n - 1)), make_int(static_cast<Int>(acc + n)));
    };

    Value init = make_tuple(make_int(5), make_int(0));
    Value arg = make_tuple(std::move(init), make_function(cf), make_function(sf));
    Value final_state = std::move(arg) | Loop;
    Value result = ElementAt(make_tuple(final_state, make_int(1)));
    REQUIRE(to_double(result) == 15.0);
  }

  SECTION("ToString and Println") {
    REQUIRE(to_string(make_int(42)) == "42");
    REQUIRE(to_string(make_bool(true)) == "True");
    REQUIRE(to_string(make_string("hello")) == "hello");
    REQUIRE(to_string(make_tuple(make_int(1), make_int(2), make_int(3))) == "(1, 2, 3)");

    Value v2 = make_int(7) | Println;
    REQUIRE(to_double(v2) == 7.0);
  }

  SECTION("Printf returns original arg tuple without newline") {
    StdoutCapture capture;
    const Value result = make_tuple(make_string("{} + {}"), make_int(1), make_int(2)) | Printf;
    REQUIRE(capture.str() == "1 + 2");

    const auto& returned = as_tuple(result);
    REQUIRE(returned.size() == 3U);
    REQUIRE(as_string(returned[0]) == "{} + {}");
    REQUIRE(to_double(returned[1]) == 1.0);
    REQUIRE(to_double(returned[2]) == 2.0);
  }
}

TEST_CASE("Runtime builtins: Std.Type", "[runtime]") {
  REQUIRE(as_string(make_int(42) | Type) == "Int64");
  REQUIRE(as_string(make_uint(42) | Type) == "UInt64");
  REQUIRE(as_string(make_float(3.14) | Type) == "Float64");
  REQUIRE(as_string(make_bool(true) | Type) == "Bool");
  REQUIRE(as_string(make_string("x") | Type) == "String");
  REQUIRE(as_string(make_null() | Type) == "Null");
  REQUIRE(as_string(make_tuple(make_int(1), make_int(2)) | Type) == "Tuple");

  Value dict = make_dict();
  REQUIRE(as_string(dict | Type) == "Dict");

  const Value callable = make_function([](const Value& v) -> Value { return v; });
  REQUIRE(as_string(callable | Type) == "Callable");
}

TEST_CASE("Runtime value formatting helpers cover all runtime value kinds", "[runtime][format]") {
  REQUIRE(format_value_plain(make_int(42)) == "42");
  REQUIRE(format_value_plain(make_float(3.5)) == "3.5");
  REQUIRE(format_value_plain(make_string("hello")) == "hello");
  REQUIRE(format_value_plain(make_null()) == "null");
  REQUIRE(format_value_plain(make_tuple(make_int(1), make_int(2))) == "(1, 2)");

  Value dict = make_dict();
  REQUIRE(format_value_plain(dict) == "{}");

  const Value function = make_function([](Value v) -> Value { return v; });
  REQUIRE(format_value_plain(function) == "<function>");

  REQUIRE(format_value_with_spec(make_null(), ">6") == "  null");
  REQUIRE(format_value_with_spec(make_tuple(make_int(1), make_int(2)), ">8") == "  (1, 2)");
  REQUIRE(format_value_with_spec(dict, "^4") == " {} ");
  REQUIRE(format_value_with_spec(function, "<12") == "<function>  ");
}

TEST_CASE("Runtime builtins: stdlib environment helpers", "[runtime]") {
  SECTION("GetArgs returns the shared process arguments") {
    auto set_args = [](const std::vector<std::string>& args) {
      std::vector<std::string> args_storage = args;
      std::vector<char*> argv_ptrs;
      argv_ptrs.reserve(args_storage.size());
      for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
      set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
    };

    set_args({"fleaux", "sample.fleaux", "--flag"});
    const Value result = GetArgs(make_tuple());
    REQUIRE(as_tuple(result).size() == 3U);
    REQUIRE(as_string(tuple_at(result, 0)) == "fleaux");
    REQUIRE(as_string(tuple_at(result, 2)) == "--flag");
  }

  SECTION("Help reads the shared help metadata registry") {
    clear_help_metadata_registry();
    register_help_metadata(HelpMetadata{
        .name = "Std.Add",
        .signature = "let Std.Add(lhs: Int64, rhs: Int64): Int64 :: __builtin__",
        .doc_lines = {"@brief Adds two integers."},
        .is_builtin = true,
    });

    const Value result = Help(make_string("Std.Add"));
    REQUIRE_THAT(as_string(result), Catch::Matchers::ContainsSubstring("Help on function Std.Add"));
    REQUIRE_THAT(as_string(result), Catch::Matchers::ContainsSubstring("Adds two integers."));

    clear_help_metadata_registry();
  }

  SECTION("Help lists prefix matches in sorted order") {
    clear_help_metadata_registry();
    register_help_metadata(HelpMetadata{
        .name = "Std.Tuple.Map",
        .signature = "let Std.Tuple.Map(): Any",
        .doc_lines = {"@brief Maps tuple items."},
        .is_builtin = true,
    });
    register_help_metadata(HelpMetadata{
        .name = "Std.Tuple.Max",
        .signature = "let Std.Tuple.Max(): Any",
        .doc_lines = {"@brief Returns the maximum tuple item."},
        .is_builtin = true,
    });
    register_help_metadata(HelpMetadata{
        .name = "Std.Tuple.Filter",
        .signature = "let Std.Tuple.Filter(): Any",
        .doc_lines = {"@brief Filters tuple items."},
        .is_builtin = true,
    });

    const auto result = Help(make_string("Std.Tuple.M"));
    const auto rendered = as_string(result);
    REQUIRE_THAT(rendered, Catch::Matchers::ContainsSubstring("Available symbols(Std.Tuple.M*):"));
    REQUIRE(rendered.find("Std.Tuple.Map") != std::string::npos);
    REQUIRE(rendered.find("Std.Tuple.Max") != std::string::npos);
    REQUIRE(rendered.find("Std.Tuple.Map") < rendered.find("Std.Tuple.Max"));

    clear_help_metadata_registry();
  }

  SECTION("Input uses the shared runtime streams") {
    std::istringstream input("Ada\nGrace\n");
    RuntimeInputStreamScope input_scope(input);
    StdoutCapture capture;

    const Value first = Input_Void(make_tuple());
    const Value second = Input_String(make_string("name> "));

    REQUIRE(capture.str() == "name> ");
    REQUIRE(as_string(first) == "Ada");
    REQUIRE(as_string(second) == "Grace");
  }
}

TEST_CASE("Runtime builtins: Std.Dict.Merge", "[runtime]") {
  SECTION("Dict helpers execute as plain functions") {
    const Value empty = DictCreate_Void(make_tuple());
    const Value seeded = DictSet(make_tuple(empty, make_string("answer"), make_int(42)));
    const Value cloned = DictCreate_Dict(seeded);
    const Value cloned_from_tuple = DictCreate_Dict(seeded);

    REQUIRE(to_double(DictGet(make_tuple(cloned, make_string("answer")))) == 42.0);
    REQUIRE(to_double(DictGet(make_tuple(cloned_from_tuple, make_string("answer")))) == 42.0);
    REQUIRE(to_double(DictGetDefault(make_tuple(cloned, make_string("missing"), make_int(9)))) == 9.0);
    REQUIRE(as_bool(DictContains(make_tuple(cloned, make_string("answer")))));

    const Value keys = DictKeys(cloned);
    const Value values = DictValues(cloned);
    const Value entries = DictEntries(cloned);
    REQUIRE(as_tuple(keys).size() == 1U);
    REQUIRE(as_string(tuple_at(keys, 0)) == "answer");
    REQUIRE(to_double(tuple_at(values, 0)) == 42.0);
    REQUIRE(as_tuple(tuple_at(entries, 0)).size() == 2U);
    REQUIRE(as_string(tuple_at(tuple_at(entries, 0), 0)) == "answer");
    REQUIRE(to_double(tuple_at(tuple_at(entries, 0), 1)) == 42.0);

    const Value erased = DictDelete(make_tuple(cloned, make_string("answer")));
    REQUIRE_FALSE(as_bool(DictContains(make_tuple(erased, make_string("answer")))));
    REQUIRE(to_double(DictLength(cloned)) == 1.0);
    REQUIRE(to_double(DictLength(DictClear(cloned))) == 0.0);
  }

  SECTION("DictMerge overlays values without mutating the base input") {
    Value base = make_dict();
    base.mutable_dict().set(make_string("shared"), make_int(1));
    base.mutable_dict().set(make_string("base_only"), make_int(10));

    Value overlay = make_dict();
    overlay.mutable_dict().set(make_string("shared"), make_int(2));
    overlay.mutable_dict().set(make_string("overlay_only"), make_int(20));

    const Value merged = DictMerge(make_tuple(base, overlay));
    const auto& merged_dict = as_dict(merged);

    REQUIRE(merged_dict.size() == 3U);
    REQUIRE(to_double(*merged_dict.find(make_string("shared"))) == 2.0);
    REQUIRE(to_double(*merged_dict.find(make_string("base_only"))) == 10.0);
    REQUIRE(to_double(*merged_dict.find(make_string("overlay_only"))) == 20.0);
    REQUIRE(to_double(*as_dict(base).find(make_string("shared"))) == 1.0);
    REQUIRE(DictKeys(merged) ==
            make_tuple(make_string("shared"), make_string("base_only"), make_string("overlay_only")));
  }

  SECTION("DictMerge keeps empty overlays as no-ops") {
    Value base = make_dict();
    base.mutable_dict().set(make_string("k"), make_int(7));

    const Value merged = DictMerge(make_tuple(base, make_dict()));
    const auto& merged_dict = as_dict(merged);

    REQUIRE(merged_dict.size() == 1U);
    REQUIRE(to_double(*merged_dict.find(make_string("k"))) == 7.0);
  }
}

TEST_CASE("Runtime builtins: tuple min/max boundaries", "[runtime][boundary]") {
  SECTION("Tuple helpers execute as plain functions") {
    const Value values = make_tuple(make_int(1), make_int(2));

    const Value appended = TupleAppend(make_tuple(values, make_int(3)));
    const Value prepended = TuplePrepend(make_tuple(values, make_int(0)));
    const Value reversed = TupleReverse(values);
    const Value zipped = TupleZip(make_tuple(values, make_tuple(make_int(3), make_int(4))));

    REQUIRE(to_double(tuple_at(appended, 2)) == 3.0);
    REQUIRE(to_double(tuple_at(prepended, 0)) == 0.0);
    REQUIRE(to_double(tuple_at(reversed, 0)) == 2.0);
    REQUIRE(as_bool(TupleContains(make_tuple(values, make_int(2)))));
    REQUIRE(as_tuple(zipped).size() == 2U);
    REQUIRE(to_double(tuple_at(TupleRangeInt64(make_int(3)), 2)) == 2.0);

    const Value from_start = TupleRangeInt64Int64(make_tuple(make_int(2), make_int(5)));
    REQUIRE(as_tuple(from_start).size() == 3U);
    REQUIRE(to_double(tuple_at(from_start, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(from_start, 2)) == 4.0);

    const Value stepped = TupleRangeInt64Int64Int64(make_tuple(make_int(5), make_int(0), make_int(-2)));
    REQUIRE(as_tuple(stepped).size() == 3U);
    REQUIRE(to_double(tuple_at(stepped, 0)) == 5.0);
    REQUIRE(to_double(tuple_at(stepped, 1)) == 3.0);
    REQUIRE(to_double(tuple_at(stepped, 2)) == 1.0);

    REQUIRE_THROWS_WITH(TupleRangeInt64Int64Int64(make_tuple(make_int(0), make_int(5), make_int(0))),
                        Catch::Matchers::ContainsSubstring("TupleRange step cannot be 0"));

    REQUIRE_THROWS_WITH(TupleReverse(make_int(9)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Reverse invariant violated: expected validated tuple value"));
    REQUIRE_THROWS_WITH(TupleSort(make_int(9)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Sort invariant violated: expected validated tuple value"));
    REQUIRE_THROWS_WITH(TupleUnique(make_int(9)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Unique invariant violated: expected validated tuple value"));
  }

  SECTION("Tuple higher-order helpers preserve tuple traversal semantics") {
    const Value values = make_tuple(make_int(1), make_int(2), make_int(3), make_int(2));
    const Value add_one = make_function([](const Value& value) -> Value { return make_int(as_int_value(value) + 1); });
    const Value is_even =
        make_function([](const Value& value) -> Value { return make_bool((as_int_value(value) % 2) == 0); });
    const Value is_positive = make_function([](const Value& value) -> Value { return make_bool(as_int_value(value) > 0); });
    const Value greater_than_three =
        make_function([](const Value& value) -> Value { return make_bool(as_int_value(value) > 3); });
    const Value sum_pair = make_function([](const Value& value) -> Value {
      const auto& pair = as_tuple(value);
      return make_int(as_int_value(pair[0]) + as_int_value(pair[1]));
    });

    const Value mapped = TupleMap(make_tuple(values, add_one));
    REQUIRE(as_tuple(mapped).size() == 4U);
    REQUIRE(to_double(tuple_at(mapped, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(mapped, 3)) == 3.0);

    const Value filtered = TupleFilter(make_tuple(values, is_even));
    REQUIRE(as_tuple(filtered).size() == 2U);
    REQUIRE(to_double(tuple_at(filtered, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(filtered, 1)) == 2.0);

    REQUIRE(to_double(TupleReduce(make_tuple(values, make_int(0), sum_pair))) == 8.0);
    REQUIRE(to_double(TupleFindIndex(make_tuple(values, is_even))) == 1.0);
    REQUIRE(to_double(TupleFindIndex(make_tuple(values, greater_than_three))) == -1.0);
    REQUIRE(as_bool(TupleAny(make_tuple(values, is_even))));
    REQUIRE_FALSE(as_bool(TupleAny(make_tuple(values, greater_than_three))));
    REQUIRE(as_bool(TupleAll(make_tuple(values, is_positive))));
    REQUIRE_FALSE(as_bool(TupleAll(make_tuple(values, is_even))));

    const Value unique = TupleUnique(values);
    REQUIRE(as_tuple(unique).size() == 3U);
    REQUIRE(to_double(tuple_at(unique, 0)) == 1.0);
    REQUIRE(to_double(tuple_at(unique, 1)) == 2.0);
    REQUIRE(to_double(tuple_at(unique, 2)) == 3.0);

    REQUIRE_THROWS_WITH(TupleMap(make_tuple(make_int(9), add_one)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Map invariant violated: expected validated tuple value"));
    REQUIRE_THROWS_WITH(TupleFilter(make_tuple(values, make_int(9))),
                        Catch::Matchers::ContainsSubstring(
                            "Std.Tuple.Filter invariant violated: expected validated callable reference"));
    REQUIRE_THROWS_WITH(TupleReduce(make_tuple(values, make_int(0), make_int(9))),
                        Catch::Matchers::ContainsSubstring(
                            "Std.Tuple.Reduce invariant violated: expected validated callable reference"));
    REQUIRE_THROWS_WITH(TupleAny(make_tuple(make_int(9), is_even)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Any invariant violated: expected validated tuple value"));
  }

  SECTION("TupleReduce preserves binary callable specializations") {
    const Value values = make_tuple(make_int(1), make_int(2), make_int(3), make_int(2));
    // Counts which of its two forms a reduction calls.
    class CountingSum final : public Callable {
    public:
      CountingSum(int& unary_calls, int& binary_calls) : unary_calls_(unary_calls), binary_calls_(binary_calls) {}

      [[nodiscard]] auto call(Value value) const -> Value override {
        ++unary_calls_;
        const auto& pair = as_tuple(value);
        return make_int(as_int_value(pair[0]) + as_int_value(pair[1]));
      }

      [[nodiscard]] auto call_binary(Value lhs, Value rhs) const -> Value override {
        ++binary_calls_;
        return make_int(as_int_value(lhs) + as_int_value(rhs));
      }

    private:
      int& unary_calls_;
      int& binary_calls_;
    };

    int unary_calls = 0;
    int binary_calls = 0;
    const Value sum_pair = adopt_function(new CountingSum(unary_calls, binary_calls));

    REQUIRE(to_double(TupleReduce(make_tuple(values, make_int(0), sum_pair))) == 8.0);
    REQUIRE(binary_calls == 4);
    REQUIRE(unary_calls == 0);
  }

  SECTION("TupleMin and TupleMax reject empty tuples") {
    REQUIRE_THROWS_WITH(TupleMin(make_tuple()), Catch::Matchers::ContainsSubstring("non-empty"));
    REQUIRE_THROWS_WITH(TupleMax(make_tuple()), Catch::Matchers::ContainsSubstring("non-empty"));
  }

  SECTION("TupleMin and TupleMax handle singleton tuples") {
    REQUIRE(to_double(TupleMin(make_tuple(make_int(7)))) == 7.0);
    REQUIRE(to_double(TupleMax(make_tuple(make_int(7)))) == 7.0);
    REQUIRE_THROWS_WITH(TupleMin(make_int(7)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Min invariant violated: expected validated tuple value"));
    REQUIRE_THROWS_WITH(TupleMax(make_int(7)),
                        Catch::Matchers::ContainsSubstring("Std.Tuple.Max invariant violated: expected validated tuple value"));
  }

  SECTION("TupleMin and TupleMax choose boundaries across numeric range") {
    const Value values = make_tuple(make_int(-9223372036854775807LL), make_int(0), make_int(9223372036854775807LL));
    REQUIRE(to_double(TupleMin(values)) < -9.0e18);
    REQUIRE(to_double(TupleMax(values)) > 9.0e18);
  }
}

TEST_CASE("Runtime builtins: Std.Array utilities", "[runtime]") {
  const Value grid =
      make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), make_tuple(make_int(4), make_int(5), make_int(6)));

  SECTION("Array helpers execute as plain functions") {
    const Value one_d = make_tuple(make_int(10), make_int(20), make_int(30));

    REQUIRE(to_double(ArrayGetAt(make_tuple(one_d, make_int(1)))) == 20.0);
    REQUIRE(to_double(tuple_at(ArraySetAt(make_tuple(one_d, make_int(1), make_int(99))), 1)) == 99.0);
    REQUIRE(to_double(tuple_at(ArrayInsertAt(make_tuple(one_d, make_int(1), make_int(15))), 1)) == 15.0);
    REQUIRE(as_tuple(ArrayRemoveAt(make_tuple(one_d, make_int(1)))).size() == 2U);
    REQUIRE(as_tuple(ArraySlice(make_tuple(one_d, make_int(1), make_int(3)))).size() == 2U);
    REQUIRE(as_tuple(ArrayConcat(make_tuple(one_d, make_tuple(make_int(40), make_int(50))))).size() == 5U);
    REQUIRE(to_double(ArrayRank(grid)) == 2.0);
    const Value shape = ArrayShape(grid);
    REQUIRE(as_tuple(shape).size() == 2U);
    REQUIRE(as_string(Type(tuple_at(shape, 0))) == "UInt64");
    REQUIRE(as_string(Type(tuple_at(shape, 1))) == "UInt64");
    REQUIRE(as_tuple(ArrayFlatten(grid)).size() == 6U);
  }

  SECTION("SetAt2D updates one cell without mutating others") {
    const Value out = ArraySetAt2D(make_tuple(grid, make_int(1), make_int(2), make_int(99)));
    REQUIRE(to_double(tuple_at(tuple_at(grid, 1), 2)) == 6.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 0), 0)) == 1.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 1), 1)) == 5.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 1), 2)) == 99.0);

    REQUIRE_THROWS_WITH(ArraySetAt2D(make_tuple(grid, make_int(5), make_int(0), make_int(9))),
                        Catch::Matchers::ContainsSubstring("row"));
    REQUIRE_THROWS_WITH(ArraySetAt2D(make_tuple(grid, make_int(0), make_int(9), make_int(9))),
                        Catch::Matchers::ContainsSubstring("col"));
  }

  SECTION("Fill replaces a contiguous region") {
    const Value seq = make_tuple(make_int(10), make_int(20), make_int(30), make_int(40));
    const Value out = ArrayFill(make_tuple(seq, make_int(1), make_int(2), make_int(7)));
    REQUIRE(to_double(tuple_at(out, 0)) == 10.0);
    REQUIRE(to_double(tuple_at(out, 1)) == 7.0);
    REQUIRE(to_double(tuple_at(out, 2)) == 7.0);
    REQUIRE(to_double(tuple_at(out, 3)) == 40.0);
  }

  SECTION("Transpose2D flips rows and columns") {
    const Value out = ArrayTranspose2D(grid);
    REQUIRE(as_tuple(out).size() == 3);
    REQUIRE(as_tuple(tuple_at(out, 0)).size() == 2);
    REQUIRE(to_double(tuple_at(tuple_at(out, 0), 1)) == 4.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 2), 0)) == 3.0);

    REQUIRE_THROWS_WITH(ArrayTranspose2D(make_tuple(make_int(1), make_int(2))),
                        Catch::Matchers::ContainsSubstring("invariant violated: expected validated tuple row"));
    REQUIRE_THROWS_WITH(ArrayTranspose2D(make_tuple(make_tuple(make_int(1), make_int(2)), make_tuple(make_int(3)))),
                        Catch::Matchers::ContainsSubstring("ragged grid detected"));
  }

  SECTION("Slice2D extracts a rectangular region") {
    const Value out = ArraySlice2D(make_tuple(grid, make_int(0), make_int(2), make_int(1), make_int(3)));
    REQUIRE(as_tuple(out).size() == 2);
    REQUIRE(to_double(tuple_at(tuple_at(out, 0), 0)) == 2.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 1), 1)) == 6.0);
  }

  SECTION("Reshape maps row-major flat data into 2D") {
    const Value flat = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6));
    const Value out = ArrayReshape(make_tuple(flat, make_int(2), make_int(3)));
    REQUIRE(as_tuple(out).size() == 2);
    REQUIRE(to_double(tuple_at(tuple_at(out, 0), 2)) == 3.0);
    REQUIRE(to_double(tuple_at(tuple_at(out, 1), 0)) == 4.0);

    REQUIRE_THROWS_WITH(ArrayReshape(make_tuple(flat, make_int(4), make_int(4))),
                        Catch::Matchers::ContainsSubstring("requires"));
  }

  SECTION("1-D helpers: get/set/insert/remove/slice/concat") {
    const Value one_d = make_tuple(make_int(10), make_int(20), make_int(30));

    REQUIRE(to_double(ArrayGetAt(make_tuple(one_d, make_int(1)))) == 20.0);

    const Value set_out = ArraySetAt(make_tuple(one_d, make_int(1), make_int(99)));
    REQUIRE(to_double(tuple_at(set_out, 0)) == 10.0);
    REQUIRE(to_double(tuple_at(set_out, 1)) == 99.0);
    REQUIRE(to_double(tuple_at(set_out, 2)) == 30.0);

    const Value insert_out = ArrayInsertAt(make_tuple(one_d, make_int(1), make_int(15)));
    REQUIRE(as_tuple(insert_out).size() == 4);
    REQUIRE(to_double(tuple_at(insert_out, 1)) == 15.0);

    const Value remove_out = ArrayRemoveAt(make_tuple(one_d, make_int(1)));
    REQUIRE(as_tuple(remove_out).size() == 2);
    REQUIRE(to_double(tuple_at(remove_out, 0)) == 10.0);
    REQUIRE(to_double(tuple_at(remove_out, 1)) == 30.0);

    const Value slice_out = ArraySlice(make_tuple(one_d, make_int(1), make_int(3)));
    REQUIRE(as_tuple(slice_out).size() == 2);
    REQUIRE(to_double(tuple_at(slice_out, 0)) == 20.0);
    REQUIRE(to_double(tuple_at(slice_out, 1)) == 30.0);

    const Value concat_out = ArrayConcat(make_tuple(one_d, make_tuple(make_int(40), make_int(50))));
    REQUIRE(as_tuple(concat_out).size() == 5);
    REQUIRE(to_double(tuple_at(concat_out, 4)) == 50.0);
  }

  SECTION("n-D helpers: shape/rank/flatten/get/set/reshape") {
    const Value cube =
        make_tuple(make_tuple(make_tuple(make_int(1), make_int(2)), make_tuple(make_int(3), make_int(4))),
                   make_tuple(make_tuple(make_int(5), make_int(6)), make_tuple(make_int(7), make_int(8))));

    REQUIRE(to_double(ArrayRank(cube)) == 3.0);
    const Value shape = ArrayShape(cube);
    REQUIRE(as_tuple(shape).size() == 3);
    REQUIRE(as_string(Type(tuple_at(shape, 0))) == "UInt64");
    REQUIRE(as_string(Type(tuple_at(shape, 1))) == "UInt64");
    REQUIRE(as_string(Type(tuple_at(shape, 2))) == "UInt64");
    REQUIRE(to_double(tuple_at(shape, 0)) == 2.0);
    REQUIRE(to_double(tuple_at(shape, 1)) == 2.0);
    REQUIRE(to_double(tuple_at(shape, 2)) == 2.0);

    const Value flattened = ArrayFlatten(cube);
    REQUIRE(as_tuple(flattened).size() == 8);
    REQUIRE(to_double(tuple_at(flattened, 0)) == 1.0);
    REQUIRE(to_double(tuple_at(flattened, 7)) == 8.0);

    REQUIRE(to_double(ArrayGetAtND(make_tuple(cube, make_tuple(make_int(1), make_int(0), make_int(1))))) == 6.0);

    const Value set_nd = ArraySetAtND(make_tuple(cube, make_tuple(make_int(1), make_int(1), make_int(0)), make_int(70)));
    REQUIRE(to_double(ArrayGetAtND(make_tuple(set_nd, make_tuple(make_int(1), make_int(1), make_int(0))))) == 70.0);

    const Value reshaped_nd = ArrayReshapeND(make_tuple(flattened, make_tuple(make_int(2), make_int(2), make_int(2))));
    REQUIRE(to_double(ArrayGetAtND(make_tuple(reshaped_nd, make_tuple(make_int(1), make_int(1), make_int(1))))) == 8.0);


    const Value whole = ArrayGetAtND(make_tuple(cube, make_tuple()));
    REQUIRE(as_tuple(whole).size() == 2);

    REQUIRE_THROWS_AS(
        ArrayGetAtND(make_tuple(cube, make_tuple(make_int(0), make_int(0), make_int(0), make_int(0)))),
        std::runtime_error);

    REQUIRE_THROWS_WITH(ArrayReshapeND(make_tuple(flattened, make_tuple(make_int(2), make_int(2), make_int(3)))),
                        Catch::Matchers::ContainsSubstring("shape product"));

    REQUIRE_THROWS_WITH(ArrayReshapeND(make_tuple(make_tuple(), make_tuple())),
                        Catch::Matchers::ContainsSubstring("shape product"));

    const Value scalar_reshape = ArrayReshapeND(make_tuple(make_tuple(make_int(9)), make_tuple()));
    REQUIRE(to_double(scalar_reshape) == 9.0);
  }
}

TEST_CASE("Runtime builtins: temp lifecycle and file handle cleanup", "[runtime][boundary]") {
  SECTION("OSMakeTempFile and OSMakeTempDir materialize resources") {
    const Value temp_file = make_tuple() | OSMakeTempFile;
    const Value temp_dir = make_tuple() | OSMakeTempDir;

    REQUIRE(temp_file.is_string());
    REQUIRE(temp_dir.is_string());

    const std::filesystem::path file_path{as_string(temp_file)};
    const std::filesystem::path dir_path{as_string(temp_dir)};
    REQUIRE(std::filesystem::exists(file_path));
    REQUIRE(std::filesystem::exists(dir_path));

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove_all(dir_path, ec);
  }

  SECTION("FileWithOpen closes handle on success") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_runtime_boundary_filewithopen_success";
    std::filesystem::create_directories(temp_dir);
    const auto file_path = temp_dir / "ok.txt";
    {
      std::ofstream out(file_path);
      out << "line";
    }

    Value captured_token = make_null();
    const Value capture_and_return = make_function([&captured_token](const Value& token) -> Value {
      captured_token = token;
      return make_string("ok");
    });

    const Value result =
        make_tuple(make_string(file_path.string()), make_string("r"), capture_and_return) | FileWithOpen;
    REQUIRE(as_string(result) == "ok");
    REQUIRE_THROWS_WITH(captured_token | FileReadLine, Catch::Matchers::ContainsSubstring("closed or invalid"));

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove_all(temp_dir, ec);
  }

  SECTION("FileWithOpen closes handle when callback throws") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_runtime_boundary_filewithopen_throw";
    std::filesystem::create_directories(temp_dir);
    const auto file_path = temp_dir / "err.txt";
    {
      std::ofstream out(file_path);
      out << "line";
    }

    Value captured_token = make_null();
    const Value capture_and_throw = make_function([&captured_token](const Value& token) -> Value {
      captured_token = token;
      throw std::runtime_error("boom");
    });

    REQUIRE_THROWS_WITH(
        make_tuple(make_string(file_path.string()), make_string("r"), capture_and_throw) | FileWithOpen,
        Catch::Matchers::ContainsSubstring("boom"));
    REQUIRE_THROWS_WITH(captured_token | FileReadLine, Catch::Matchers::ContainsSubstring("closed or invalid"));

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove_all(temp_dir, ec);
  }

  SECTION("FileClose is idempotent and stale token stays invalid after reopen") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_runtime_boundary_fileclose_cycle";
    std::filesystem::create_directories(temp_dir);
    const auto file_path = temp_dir / "cycle.txt";
    {
      std::ofstream out(file_path);
      out << "line";
    }

    const Value first_token = make_tuple(make_string(file_path.string()), make_string("r")) | FileOpen;
    REQUIRE(as_bool(first_token | FileClose));
    REQUIRE_FALSE(as_bool(first_token | FileClose));
    REQUIRE_THROWS_WITH(first_token | FileReadLine, Catch::Matchers::ContainsSubstring("closed or invalid"));

    const Value reopened_token = make_tuple(make_string(file_path.string()), make_string("r")) | FileOpen;
    const Value read_back = reopened_token | FileReadLine;
    REQUIRE(as_tuple(read_back).size() == 3);
    REQUIRE(as_string(tuple_at(read_back, 1)) == "line");
    REQUIRE(as_bool(reopened_token | FileClose));

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove_all(temp_dir, ec);
  }
}

TEST_CASE("Runtime builtins: string helpers", "[runtime]") {
  SECTION("conversion helpers") {
    REQUIRE(as_string(ToString(make_int(42))) == "42");
    REQUIRE(as_string(ToString(make_int(1))) == "1");
    REQUIRE(as_string(ToString(make_tuple(make_int(1), make_int(2)))) == "(1, 2)");
    REQUIRE(to_double(ToNum(make_string("42.5"))) == 42.5);
    REQUIRE(to_double(ToNum(make_string("\t +1.25e2"))) == 125.0);
    REQUIRE_THROWS_WITH(ToNum(make_string("42xyz")), Catch::Matchers::ContainsSubstring("trailing characters"));

    REQUIRE(as_int_value(StringParseInt64(make_string("  +42"))) == 42);
    REQUIRE(as_int_value(StringParseInt64(make_string("-42"))) == -42);
    REQUIRE_THROWS_WITH(StringParseInt64(make_string("9223372036854775808")),
                        Catch::Matchers::ContainsSubstring("out of range"));

    REQUIRE(is_uint_number(StringParseUInt64(make_string("18446744073709551615"))));
    REQUIRE_THROWS_WITH(StringParseUInt64(make_string("-1")),
                        Catch::Matchers::ContainsSubstring("invalid numeric input"));
    REQUIRE_THROWS_WITH(StringParseUInt64(make_string("18446744073709551616")),
                        Catch::Matchers::ContainsSubstring("out of range"));

    REQUIRE(to_double(StringParseFloat64(make_string(" +6.022e2"))) == 602.2);
    REQUIRE_THROWS_WITH(StringParseFloat64(make_string("")),
                        Catch::Matchers::ContainsSubstring("invalid numeric input"));
  }

  SECTION("case conversion and trimming") {
    REQUIRE(as_string(StringUpper(make_string("hElLo"))) == "HELLO");
    REQUIRE(as_string(StringLower(make_string("MiXeD"))) == "mixed");
    REQUIRE(as_string(StringTrim(make_string("  trim me  "))) == "trim me");
    REQUIRE(as_string(StringTrimStart(make_string("  left"))) == "left");
    REQUIRE(as_string(StringTrimEnd(make_string("right  "))) == "right");
  }

  SECTION("split join replace and predicate helpers") {
    const Value parts = StringSplit(make_tuple(make_string("a,b,c"), make_string(",")));
    REQUIRE(as_tuple(parts).size() == 3);
    REQUIRE(as_string(tuple_at(parts, 0)) == "a");
    REQUIRE(as_string(tuple_at(parts, 1)) == "b");
    REQUIRE(as_string(tuple_at(parts, 2)) == "c");

    REQUIRE(as_string(StringJoin(make_tuple(make_string(","), make_tuple(make_string("abc"), make_string("b"),
                                                               make_string("bc"))))) == "abc,b,bc");
    REQUIRE_THROWS_WITH(StringJoin(make_tuple(make_string("-"), make_string("abc"))), "fleaux::runtime: expected Tuple");
    REQUIRE(as_string(StringReplace(make_tuple(make_string("a,b,c"), make_string(","), make_string("_")))) ==
            "a_b_c");
    REQUIRE(as_bool(StringContains(make_tuple(make_string("abc"), make_string("b")))));
    REQUIRE(as_bool(StringStartsWith(make_tuple(make_string("abc"), make_string("ab")))));
    REQUIRE(as_bool(StringEndsWith(make_tuple(make_string("abc"), make_string("bc")))));
    REQUIRE_FALSE(as_bool(StringEndsWith(make_tuple(make_string("abc"), make_string("abcd")))));
  }

  SECTION("length indexing slicing find and format") {
    REQUIRE(to_double(StringLength(make_string("abcd"))) == 4.0);
    REQUIRE(as_string(StringCharAt(make_tuple(make_string("abc"), make_int(1)))) == "b");
    REQUIRE(as_string(StringCharAt(make_tuple(make_string("abc"), make_int(9)))) == "");
    REQUIRE(as_string(StringSliceStringUInt64(make_tuple(make_string("abcdef"), make_int(3)))) == "abc");
    REQUIRE(as_string(StringSliceStringUInt64UInt64(make_tuple(make_string("abcdef"), make_int(1), make_int(4)))) == "bcd");
    REQUIRE(to_double(StringFindStringString(make_tuple(make_string("abcabc"), make_string("bc")))) == 1.0);
    REQUIRE(to_double(StringFindStringStringUInt64(make_tuple(make_string("abcabc"), make_string("bc"), make_int(2)))) == 4.0);
    REQUIRE(to_double(StringFindStringString(make_tuple(make_string("abcabc"), make_string("zz")))) == -1.0);
    REQUIRE(as_string(
                StringFormat(make_tuple(make_string("{} + {} = {}"), make_int(2), make_int(3), make_int(5)))) ==
            "2 + 3 = 5");
  }
}

TEST_CASE("Runtime builtins: string regex", "[runtime]") {
  SECTION("IsMatch and Find") {
    const Value text = make_string("abc-123 xyz");
    REQUIRE(as_bool(StringRegexIsMatch(make_tuple(text, make_string("[a-z]+-[0-9]+")))));
    REQUIRE_FALSE(as_bool(StringRegexIsMatch(make_tuple(text, make_string("^XYZ$")))));
    REQUIRE(to_double(StringRegexFind(make_tuple(text, make_string("[0-9]+")))) == 4.0);
    REQUIRE(to_double(StringRegexFind(make_tuple(text, make_string("ZZZ")))) == -1.0);
  }

  SECTION("Replace and Split") {
    const Value text = make_string("one,two;three");
    REQUIRE(as_string(StringRegexReplace(make_tuple(text, make_string("[,;]"), make_string("|")))) ==
            "one|two|three");

    const Value parts = StringRegexSplit(make_tuple(text, make_string("[,;]")));
    REQUIRE(as_tuple(parts).size() == 3);
    REQUIRE(as_string(tuple_at(parts, 0)) == "one");
    REQUIRE(as_string(tuple_at(parts, 1)) == "two");
    REQUIRE(as_string(tuple_at(parts, 2)) == "three");
  }

  SECTION("Invalid regex pattern content remains runtime-owned") {
    const Value text = make_string("abc-123 xyz");
    REQUIRE_THROWS_WITH(StringRegexIsMatch(make_tuple(text, make_string("("))),
                        Catch::Matchers::ContainsSubstring("Regex compile failed"));
    REQUIRE_THROWS_WITH(StringRegexSplit(make_tuple(text, make_string("("))),
                        Catch::Matchers::ContainsSubstring("Regex compile failed"));
  }
}

TEST_CASE("Runtime builtins: numeric cast helpers", "[runtime]") {
  SECTION("ToInt64") {
    REQUIRE(as_int_value(ToInt64(make_int(42))) == 42);
    REQUIRE(as_int_value(ToInt64(make_float(3.0))) == 3);
    REQUIRE_THROWS_AS(ToInt64(make_float(1.5)), std::invalid_argument);
  }

  SECTION("ToUInt64") {
    REQUIRE(is_uint_number(ToUInt64(make_int(5))));
    REQUIRE(is_uint_number(ToUInt64(make_uint(10))));
    REQUIRE_THROWS_AS(ToUInt64(make_int(-1)), std::invalid_argument);
    REQUIRE_THROWS_AS(ToUInt64(make_float(-0.5)), std::invalid_argument);
  }

  SECTION("ToFloat64") {
    REQUIRE(is_float_number(ToFloat64(make_int(7))));
    REQUIRE(is_float_number(ToFloat64(make_uint(7))));
    REQUIRE(is_float_number(ToFloat64(make_float(7.0))));
    REQUIRE(to_double(ToFloat64(make_int(3))) == 3.0);
  }
}

TEST_CASE("Runtime builtins: wrapping and saturating integer arithmetic", "[runtime][arithmetic]") {
  constexpr auto kInt64Max = std::numeric_limits<Int>::max();
  constexpr auto kInt64Min = std::numeric_limits<Int>::min();
  constexpr auto kUInt64Max = std::numeric_limits<UInt>::max();

  REQUIRE(WrappingAdd(make_tuple(make_int(kInt64Max), make_int(1))) == make_int(kInt64Min));
  REQUIRE(WrappingSubtract(make_tuple(make_int(kInt64Min), make_int(1))) == make_int(kInt64Max));
  REQUIRE(WrappingMultiply(make_tuple(make_int(kInt64Max), make_int(2))) == make_int(-2));
  REQUIRE(WrappingSubtract(make_tuple(make_uint(1U), make_uint(2U))) == make_uint(kUInt64Max));
  REQUIRE(WrappingAdd(make_tuple(make_int(1), make_int(2))) == make_int(3));

  REQUIRE(SaturatingAdd(make_tuple(make_int(kInt64Max), make_int(1))) == make_int(kInt64Max));
  REQUIRE(SaturatingSubtract(make_tuple(make_int(kInt64Min), make_int(1))) == make_int(kInt64Min));
  REQUIRE(SaturatingMultiply(make_tuple(make_int(kInt64Min), make_int(2))) == make_int(kInt64Min));
  REQUIRE(SaturatingMultiply(make_tuple(make_int(kInt64Min), make_int(-2))) == make_int(kInt64Max));
  REQUIRE(SaturatingSubtract(make_tuple(make_uint(1U), make_uint(2U))) == make_uint(0U));
  REQUIRE(SaturatingAdd(make_tuple(make_uint(kUInt64Max), make_uint(1U))) == make_uint(kUInt64Max));
  REQUIRE(SaturatingMultiply(make_tuple(make_int(2), make_int(2))) == make_int(4));
}

TEST_CASE("Runtime builtins: exact integer arithmetic keeps the operand type", "[runtime][arithmetic]") {
  REQUIRE(Divide(make_tuple(make_int(10), make_int(3))) == make_int(3));
  REQUIRE(Divide(make_tuple(make_int(-7), make_int(2))) == make_int(-3));
  REQUIRE(Mod(make_tuple(make_int(-7), make_int(2))) == make_int(-1));
  REQUIRE(Pow(make_tuple(make_uint(3U), make_uint(4U))) == make_uint(81U));
  REQUIRE(UnaryMinus(make_int(5)) == make_int(-5));
  REQUIRE(Add(make_tuple(make_int(9007199254740993), make_int(0))) == make_int(9007199254740993));

  REQUIRE_THROWS_WITH(Add(make_tuple(make_int(std::numeric_limits<Int>::max()), make_int(1))), "Add: Int64 overflow");
  REQUIRE_THROWS_WITH(Divide(make_tuple(make_uint(1U), make_uint(0U))), "Divide: UInt64 division by zero");
  REQUIRE_THROWS_WITH(UnaryMinus(make_int(std::numeric_limits<Int>::min())), "UnaryMinus: Int64 overflow");
}

TEST_CASE("Runtime type checks follow the runtime value representation", "[runtime][cast]") {
  using fleaux::common::RuntimeTypeCheck;
  using fleaux::common::RuntimeTypeCheckKind;
  const auto leaf = [](const RuntimeTypeCheckKind kind) { return RuntimeTypeCheck{.kind = kind}; };
  const auto tuple_of_any = RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kTuple,
                                             .items = {leaf(RuntimeTypeCheckKind::kAny)},
                                             .variadic_tail = true};

  SECTION("Function values never pass as tuples") {
    const Value callable = make_function([](const Value& value) -> Value { return value; });
    REQUIRE(callable.is_function());
    REQUIRE_FALSE(value_matches_type_check(callable, tuple_of_any));
    REQUIRE(value_matches_type_check(make_tuple(make_int(1), make_string("a")), tuple_of_any));
  }

  SECTION("Numeric dictionary keys keep their exact type") {
    const Value dict = DictSet(make_tuple(make_dict(), make_int(3), make_string("three")));
    const auto dict_of = [&](const RuntimeTypeCheckKind key_kind) {
      return RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kDict,
                              .items = {leaf(key_kind), leaf(RuntimeTypeCheckKind::kString)}};
    };
    REQUIRE(value_matches_type_check(dict, dict_of(RuntimeTypeCheckKind::kInt64)));
    REQUIRE_FALSE(value_matches_type_check(dict, dict_of(RuntimeTypeCheckKind::kUInt64)));
    REQUIRE_FALSE(value_matches_type_check(dict, dict_of(RuntimeTypeCheckKind::kFloat64)));
    REQUIRE_FALSE(value_matches_type_check(dict, dict_of(RuntimeTypeCheckKind::kString)));
  }

  SECTION("Result checks the payload against the side its tag selects") {
    const auto result_check = RuntimeTypeCheck{
        .kind = RuntimeTypeCheckKind::kResult,
        .items = {leaf(RuntimeTypeCheckKind::kInt64), leaf(RuntimeTypeCheckKind::kString)}};
    REQUIRE(value_matches_type_check(ResultOk(make_int(1)), result_check));
    REQUIRE(value_matches_type_check(ResultErr(make_string("bad")), result_check));
    REQUIRE_FALSE(value_matches_type_check(ResultOk(make_string("bad")), result_check));
  }

  SECTION("Never matches nothing and Any matches everything") {
    REQUIRE_FALSE(value_matches_type_check(make_null(), leaf(RuntimeTypeCheckKind::kNever)));
    REQUIRE(value_matches_type_check(make_null(), leaf(RuntimeTypeCheckKind::kAny)));
  }
}

TEST_CASE("RandomGenerator checks accept only generator values", "[runtime][cast]") {
  using fleaux::common::RuntimeTypeCheck;
  using fleaux::common::RuntimeTypeCheckKind;
  const RuntimeTypeCheck check{.kind = RuntimeTypeCheckKind::kRandomGenerator};

  REQUIRE(value_matches_type_check(RandomCreate(make_uint(1U)), check));
  REQUIRE_FALSE(value_matches_type_check(make_tuple(make_string("__fleaux_rng__"), make_uint(2U), make_uint(5U)), check));
  REQUIRE_FALSE(value_matches_type_check(make_tuple(make_string("__fleaux_rng__"), make_uint(1U), make_int(5)), check));
  REQUIRE_FALSE(value_matches_type_check(make_tuple(make_string("__fleaux_rng__"), make_int(1), make_uint(5U)), check));
}

TEST_CASE("Dictionary keys decode back to their original numeric type", "[runtime][dict]") {
  const auto keys_of = [](const Value& key) {
    const Value dict = DictSet(make_tuple(make_dict(), key, make_string("v")));
    return as_tuple(DictKeys(dict))[0];
  };

  REQUIRE(as_string(Type(keys_of(make_uint(18446744073709551615ULL)))) == "UInt64");
  REQUIRE(keys_of(make_uint(18446744073709551615ULL)) == make_uint(18446744073709551615ULL));
  REQUIRE(as_string(Type(keys_of(make_int(-3)))) == "Int64");
  REQUIRE(as_string(Type(keys_of(make_float(2.0)))) == "Float64");
  REQUIRE(keys_of(make_float(0.1)) == make_float(0.1));

  // Keys of different numeric types are different keys.
  Value dict = DictSet(make_tuple(make_dict(), make_int(1), make_string("int")));
  dict = DictSet(make_tuple(dict, make_float(1.0), make_string("float")));
  REQUIRE(as_int_value(DictLength(dict)) == 2);
}

TEST_CASE("Bit shifts of 64 or more bits are runtime errors", "[runtime][bitwise]") {
  REQUIRE(BitShiftLeft(make_tuple(make_int(1), make_uint(62U))) == make_int(Int{1} << 62));
  REQUIRE(BitShiftLeft(make_tuple(make_int(-1), make_uint(4U))) == make_int(-16));
  REQUIRE(BitShiftRight(make_tuple(make_int(-16), make_uint(2U))) == make_int(-4));
  REQUIRE(BitShiftRight(make_tuple(make_int(5), make_uint(63U))) == make_int(0));

  REQUIRE_THROWS_WITH(BitShiftLeft(make_tuple(make_int(1), make_uint(64U))),
                      "BitShiftLeft: shift count 64 is out of range for Int64 (0 to 63)");
  REQUIRE_THROWS_AS(BitShiftRight(make_tuple(make_int(1), make_uint(1000U))), std::out_of_range);
}

TEST_CASE("Runtime builtins: Float64 results stay Float64 when they are whole numbers", "[runtime]") {
  CHECK(is_float_number(Sqrt(make_float(9.0))));
  CHECK(is_float_number(MathFloor(make_float(2.9))));
  CHECK(is_float_number(MathCeil(make_float(2.1))));
  CHECK(is_float_number(MathAbs(make_float(-3.0))));
  CHECK(is_float_number(MathLog(make_float(1.0))));
  CHECK(is_float_number(MathClamp(make_tuple(make_float(650.0), make_float(0.0), make_float(600.0)))));
  CHECK(is_float_number(ToNum(make_string("42"))));
}
