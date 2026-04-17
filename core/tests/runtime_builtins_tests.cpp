#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

#include "fleaux/runtime/runtime_support.hpp"

using namespace fleaux::runtime;

TEST_CASE("Runtime builtins: core tuple helpers", "[runtime]") {
  SECTION("Wrap and Unwrap") {
    Value tpl = make_tuple(make_int(1), make_int(2), make_int(3));
    REQUIRE(as_array(tpl).Size() == 3);

    Value wrapped = tpl | Wrap{};
    REQUIRE(as_array(wrapped).Size() == 1);

    Value back = std::move(wrapped) | Unwrap{};
    REQUIRE(as_array(back).Size() == 3);
  }

  SECTION("ElementAt") {
    Value tpl = make_tuple(make_int(10), make_string("hello"), make_bool(true));
    Value r0 = make_tuple(tpl, make_int(0)) | ElementAt{};
    REQUIRE(to_double(r0) == 10.0);

    Value r1 = make_tuple(tpl, make_int(1)) | ElementAt{};
    REQUIRE(as_string(r1) == "hello");

    Value r2 = make_tuple(tpl, make_int(2)) | ElementAt{};
    REQUIRE(as_bool(r2));
  }

  SECTION("Length") {
    Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4));
    Value len = seq | Length{};  // Option B: Length takes array directly, no wrapper
    REQUIRE(to_double(len) == 4.0);
  }

  SECTION("Take, Drop, Slice") {
    Value seq = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5));

    Value taken = make_tuple(seq, make_int(3)) | Take{};
    REQUIRE(as_array(taken).Size() == 3);
    REQUIRE(to_double(array_at(taken, 0)) == 1.0);
    REQUIRE(to_double(array_at(taken, 2)) == 3.0);

    Value dropped = make_tuple(seq, make_int(2)) | Drop{};
    REQUIRE(as_array(dropped).Size() == 3);
    REQUIRE(to_double(array_at(dropped, 0)) == 3.0);

    Value sliced = make_tuple(seq, make_int(1), make_int(4)) | Slice{};
    REQUIRE(as_array(sliced).Size() == 3);
    REQUIRE(to_double(array_at(sliced, 0)) == 2.0);
    REQUIRE(to_double(array_at(sliced, 2)) == 4.0);
  }
}

TEST_CASE("Runtime builtins: arithmetic and logic", "[runtime]") {
  REQUIRE(to_double(make_tuple(make_int(3), make_int(4)) | Add{}) == 7.0);
  REQUIRE(to_double(make_tuple(make_int(9), make_int(4)) | Subtract{}) == 5.0);
  REQUIRE(to_double(make_tuple(make_int(3), make_int(7)) | Multiply{}) == 21.0);
  REQUIRE(to_double(make_tuple(make_float(10.0), make_float(4.0)) | Divide{}) == 2.5);
  REQUIRE(to_double(make_tuple(make_int(2), make_int(8)) | Pow{}) == 256.0);
  REQUIRE(to_double(make_tuple(make_int(10), make_int(3)) | Mod{}) == 1.0);
  REQUIRE(to_double(make_int(5) | UnaryMinus{}) == -5.0);
  REQUIRE(as_string(make_tuple(make_string("hello"), make_string(" world")) | Add{}) == "hello world");

  REQUIRE(as_bool(make_tuple(make_int(5), make_int(3)) | GreaterThan{}));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(5)) | LessThan{}));
  REQUIRE(as_bool(make_tuple(make_int(3), make_int(3)) | Equal{}));
  REQUIRE_FALSE(as_bool(make_tuple(make_int(3), make_int(3)) | NotEqual{}));
  REQUIRE_FALSE(as_bool(make_bool(true) | Not{}));
  REQUIRE(as_bool(make_tuple(make_bool(true), make_bool(false)) | Or{}));
  REQUIRE_FALSE(as_bool(make_tuple(make_bool(true), make_bool(false)) | And{}));

  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitAnd{}) == 2.0);
  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitOr{}) == 7.0);
  REQUIRE(to_double(make_tuple(make_int(6), make_int(3)) | BitXor{}) == 5.0);
  REQUIRE(to_double(make_int(0) | BitNot{}) == -1.0);
  REQUIRE(to_double(make_tuple(make_int(3), make_int(2)) | BitShiftLeft{}) == 12.0);
  REQUIRE(to_double(make_tuple(make_int(12), make_int(2)) | BitShiftRight{}) == 3.0);

  Value r = make_tuple(make_bool(true), make_int(11), make_int(22)) | Select{};
  REQUIRE(to_double(r) == 11.0);
}

TEST_CASE("Runtime builtins: OS exec", "[runtime]") {
  const Value result = make_string("printf 'fleaux_exec_ok'") | OSExec{};
  REQUIRE(as_array(result).Size() == 2);
  REQUIRE(to_double(array_at(result, 0)) == 0.0);
  REQUIRE(as_string(array_at(result, 1)) == "fleaux_exec_ok");
}

TEST_CASE("Runtime builtins: Std.Match supports predicate patterns", "[runtime]") {
  const Value is_even =
      make_callable_ref([](const Value& v) -> Value { return make_bool(static_cast<Int>(to_double(v)) % 2 == 0); });
  const Value even_handler = make_callable_ref([](const Value& _v) -> Value { return make_string("even"); });
  const Value odd_handler = make_callable_ref([](const Value& _v) -> Value { return make_string("odd"); });

  SECTION("Predicate pattern matches before wildcard") {
    Value arg = make_tuple(make_int(6), make_tuple(is_even, even_handler),
                           make_tuple(make_string("__fleaux_match_wildcard__"), odd_handler));
    Value result = std::move(arg) | Match{};
    REQUIRE(as_string(result) == "even");
  }

  SECTION("Predicate pattern falls through to wildcard") {
    Value arg = make_tuple(make_int(7), make_tuple(is_even, even_handler),
                           make_tuple(make_string("__fleaux_match_wildcard__"), odd_handler));
    Value result = std::move(arg) | Match{};
    REQUIRE(as_string(result) == "odd");
  }
}

TEST_CASE("Runtime builtins: Std.Result helpers", "[runtime]") {
  SECTION("Constructors and predicates") {
    const Value ok = make_int(42) | ResultOk{};
    REQUIRE(as_bool(ok | ResultIsOk{}));
    REQUIRE_FALSE(as_bool(ok | ResultIsErr{}));
    REQUIRE(as_bool(ok | ResultTag{}));  // true for Ok
    REQUIRE(to_double(ok | ResultPayload{}) == 42.0);

    const Value err = make_string("boom") | ResultErr{};
    REQUIRE(as_bool(err | ResultIsErr{}));
    REQUIRE_FALSE(as_bool(err | ResultIsOk{}));
    REQUIRE_FALSE(as_bool(err | ResultTag{}));  // false for Err
    REQUIRE(as_string(err | ResultPayload{}) == "boom");
  }

  SECTION("Unwrap and UnwrapErr") {
    const Value ok = make_int(9) | ResultOk{};
    const Value err = make_string("bad") | ResultErr{};

    REQUIRE(to_double(ok | ResultUnwrap{}) == 9.0);
    REQUIRE(as_string(err | ResultUnwrapErr{}) == "bad");

    REQUIRE_THROWS_WITH(err | ResultUnwrap{}, Catch::Matchers::ContainsSubstring("Result.Unwrap expected Ok"));
    REQUIRE_THROWS_WITH(ok | ResultUnwrapErr{}, Catch::Matchers::ContainsSubstring("Result.UnwrapErr expected Err"));
  }
}

TEST_CASE("Runtime builtins: Std.Try", "[runtime]") {
  SECTION("Try returns Ok when callable succeeds") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value result = make_tuple(make_int(41), add_one) | Try{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(to_double(result | ResultUnwrap{}) == 42.0);
  }

  SECTION("Try returns Err(message) when callable throws") {
    const Value boom = make_callable_ref([](const Value&) -> Value { throw std::runtime_error("boom"); });
    const Value result = make_tuple(make_int(0), boom) | Try{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    REQUIRE(as_string(result | ResultUnwrapErr{}) == "boom");
  }
}

TEST_CASE("Runtime builtins: Std.Exp.Parallel", "[runtime]") {
  SECTION("Parallel returns Ok with ordered mapped outputs") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one) | ExpParallel{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    const Value payload = result | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 3);
    REQUIRE(to_double(array_at(payload, 0)) == 2.0);
    REQUIRE(to_double(array_at(payload, 1)) == 3.0);
    REQUIRE(to_double(array_at(payload, 2)) == 4.0);
  }

  SECTION("Parallel returns Err(index, message) on first failing item") {
    const Value fail_on_two = make_callable_ref([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("boom-two"); }
      return v;
    });
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), fail_on_two) | ExpParallel{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 1.0);
    REQUIRE(as_string(array_at(err, 1)) == "boom-two");
  }

  SECTION("Parallel stress: repeated invocation remains stable and ordered") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value input = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6), make_int(7),
                             make_int(8));

    for (int iter = 0; iter < 200; ++iter) {
      const Value result = make_tuple(input, add_one) | ExpParallel{};
      REQUIRE(as_bool(result | ResultIsOk{}));
      const Value payload = result | ResultUnwrap{};
      REQUIRE(as_array(payload).Size() == 8);
      REQUIRE(to_double(array_at(payload, 0)) == 2.0);
      REQUIRE(to_double(array_at(payload, 7)) == 9.0);
    }
  }
}

TEST_CASE("Runtime builtins: loop and formatting", "[runtime]") {
  SECTION("Loop sums 1..5") {
    auto cf = [](Value state) -> Value { return make_bool(to_double(array_at(state, 0)) > 0.0); };
    auto sf = [](Value state) -> Value {
      const double n = to_double(array_at(state, 0));
      const double acc = to_double(array_at(state, 1));
      return make_tuple(make_int(static_cast<Int>(n - 1)), make_int(static_cast<Int>(acc + n)));
    };

    Value init = make_tuple(make_int(5), make_int(0));
    Value arg = make_tuple(std::move(init), make_callable_ref(cf), make_callable_ref(sf));
    Value final_state = std::move(arg) | Loop{};
    Value result = make_tuple(final_state, make_int(1)) | ElementAt{};
    REQUIRE(to_double(result) == 15.0);
  }

  SECTION("ToString and Println") {
    REQUIRE(to_string(make_int(42)) == "42");
    REQUIRE(to_string(make_bool(true)) == "True");
    REQUIRE(to_string(make_string("hello")) == "hello");
    REQUIRE(to_string(make_tuple(make_int(1), make_int(2), make_int(3))) == "(1, 2, 3)");

    Value v2 = make_int(7) | Println{};
    REQUIRE(to_double(v2) == 7.0);
  }
}

TEST_CASE("Runtime builtins: Std.Array utilities", "[runtime]") {
  const Value grid =
      make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), make_tuple(make_int(4), make_int(5), make_int(6)));

  SECTION("SetAt2D updates one cell without mutating others") {
    const Value out = make_tuple(grid, make_int(1), make_int(2), make_int(99)) | ArraySetAt2D{};
    REQUIRE(to_double(array_at(array_at(out, 0), 0)) == 1.0);
    REQUIRE(to_double(array_at(array_at(out, 1), 1)) == 5.0);
    REQUIRE(to_double(array_at(array_at(out, 1), 2)) == 99.0);

    REQUIRE_THROWS_WITH(make_tuple(grid, make_int(5), make_int(0), make_int(9)) | ArraySetAt2D{},
                        Catch::Matchers::ContainsSubstring("row"));
    REQUIRE_THROWS_WITH(make_tuple(grid, make_int(0), make_int(9), make_int(9)) | ArraySetAt2D{},
                        Catch::Matchers::ContainsSubstring("col"));
  }

  SECTION("Fill replaces a contiguous region") {
    const Value seq = make_tuple(make_int(10), make_int(20), make_int(30), make_int(40));
    const Value out = make_tuple(seq, make_int(1), make_int(2), make_int(7)) | ArrayFill{};
    REQUIRE(to_double(array_at(out, 0)) == 10.0);
    REQUIRE(to_double(array_at(out, 1)) == 7.0);
    REQUIRE(to_double(array_at(out, 2)) == 7.0);
    REQUIRE(to_double(array_at(out, 3)) == 40.0);
  }

  SECTION("Transpose2D flips rows and columns") {
    const Value out = grid | ArrayTranspose2D{};
    REQUIRE(as_array(out).Size() == 3);
    REQUIRE(as_array(array_at(out, 0)).Size() == 2);
    REQUIRE(to_double(array_at(array_at(out, 0), 1)) == 4.0);
    REQUIRE(to_double(array_at(array_at(out, 2), 0)) == 3.0);
  }

  SECTION("Slice2D extracts a rectangular region") {
    const Value out = make_tuple(grid, make_int(0), make_int(2), make_int(1), make_int(3)) | ArraySlice2D{};
    REQUIRE(as_array(out).Size() == 2);
    REQUIRE(to_double(array_at(array_at(out, 0), 0)) == 2.0);
    REQUIRE(to_double(array_at(array_at(out, 1), 1)) == 6.0);
  }

  SECTION("Reshape maps row-major flat data into 2D") {
    const Value flat = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6));
    const Value out = make_tuple(flat, make_int(2), make_int(3)) | ArrayReshape{};
    REQUIRE(as_array(out).Size() == 2);
    REQUIRE(to_double(array_at(array_at(out, 0), 2)) == 3.0);
    REQUIRE(to_double(array_at(array_at(out, 1), 0)) == 4.0);

    REQUIRE_THROWS_WITH(make_tuple(flat, make_int(4), make_int(4)) | ArrayReshape{},
                        Catch::Matchers::ContainsSubstring("requires"));
  }

  SECTION("1-D helpers: get/set/insert/remove/slice/concat") {
    const Value one_d = make_tuple(make_int(10), make_int(20), make_int(30));

    REQUIRE(to_double(make_tuple(one_d, make_int(1)) | ArrayGetAt{}) == 20.0);

    const Value set_out = make_tuple(one_d, make_int(1), make_int(99)) | ArraySetAt{};
    REQUIRE(to_double(array_at(set_out, 0)) == 10.0);
    REQUIRE(to_double(array_at(set_out, 1)) == 99.0);
    REQUIRE(to_double(array_at(set_out, 2)) == 30.0);

    const Value insert_out = make_tuple(one_d, make_int(1), make_int(15)) | ArrayInsertAt{};
    REQUIRE(as_array(insert_out).Size() == 4);
    REQUIRE(to_double(array_at(insert_out, 1)) == 15.0);

    const Value remove_out = make_tuple(one_d, make_int(1)) | ArrayRemoveAt{};
    REQUIRE(as_array(remove_out).Size() == 2);
    REQUIRE(to_double(array_at(remove_out, 0)) == 10.0);
    REQUIRE(to_double(array_at(remove_out, 1)) == 30.0);

    const Value slice_out = make_tuple(one_d, make_int(1), make_int(3)) | ArraySlice{};
    REQUIRE(as_array(slice_out).Size() == 2);
    REQUIRE(to_double(array_at(slice_out, 0)) == 20.0);
    REQUIRE(to_double(array_at(slice_out, 1)) == 30.0);

    const Value concat_out = make_tuple(one_d, make_tuple(make_int(40), make_int(50))) | ArrayConcat{};
    REQUIRE(as_array(concat_out).Size() == 5);
    REQUIRE(to_double(array_at(concat_out, 4)) == 50.0);
  }

  SECTION("n-D helpers: shape/rank/flatten/get/set/reshape") {
    const Value cube =
        make_tuple(make_tuple(make_tuple(make_int(1), make_int(2)), make_tuple(make_int(3), make_int(4))),
                   make_tuple(make_tuple(make_int(5), make_int(6)), make_tuple(make_int(7), make_int(8))));

    REQUIRE(to_double(cube | ArrayRank{}) == 3.0);
    const Value shape = cube | ArrayShape{};
    REQUIRE(as_array(shape).Size() == 3);
    REQUIRE(to_double(array_at(shape, 0)) == 2.0);
    REQUIRE(to_double(array_at(shape, 1)) == 2.0);
    REQUIRE(to_double(array_at(shape, 2)) == 2.0);

    const Value flattened = cube | ArrayFlatten{};
    REQUIRE(as_array(flattened).Size() == 8);
    REQUIRE(to_double(array_at(flattened, 0)) == 1.0);
    REQUIRE(to_double(array_at(flattened, 7)) == 8.0);

    REQUIRE(to_double(make_tuple(cube, make_tuple(make_int(1), make_int(0), make_int(1))) | ArrayGetAtND{}) == 6.0);

    const Value set_nd =
        make_tuple(cube, make_tuple(make_int(1), make_int(1), make_int(0)), make_int(70)) | ArraySetAtND{};
    REQUIRE(to_double(make_tuple(set_nd, make_tuple(make_int(1), make_int(1), make_int(0))) | ArrayGetAtND{}) == 70.0);

    const Value reshaped_nd =
        make_tuple(flattened, make_tuple(make_int(2), make_int(2), make_int(2))) | ArrayReshapeND{};
    REQUIRE(to_double(make_tuple(reshaped_nd, make_tuple(make_int(1), make_int(1), make_int(1))) | ArrayGetAtND{}) ==
            8.0);
  }
}

TEST_CASE("Runtime builtins: string regex", "[runtime]") {
  SECTION("IsMatch and Find") {
    const Value text = make_string("abc-123 xyz");
    REQUIRE(as_bool(make_tuple(text, make_string("[a-z]+-[0-9]+")) | StringRegexIsMatch{}));
    REQUIRE_FALSE(as_bool(make_tuple(text, make_string("^XYZ$")) | StringRegexIsMatch{}));
    REQUIRE(to_double(make_tuple(text, make_string("[0-9]+")) | StringRegexFind{}) == 4.0);
    REQUIRE(to_double(make_tuple(text, make_string("ZZZ")) | StringRegexFind{}) == -1.0);
  }

  SECTION("Replace and Split") {
    const Value text = make_string("one,two;three");
    REQUIRE(as_string(make_tuple(text, make_string("[,;]"), make_string("|")) | StringRegexReplace{}) ==
            "one|two|three");

    const Value parts = make_tuple(text, make_string("[,;]")) | StringRegexSplit{};
    REQUIRE(as_array(parts).Size() == 3);
    REQUIRE(as_string(array_at(parts, 0)) == "one");
    REQUIRE(as_string(array_at(parts, 1)) == "two");
    REQUIRE(as_string(array_at(parts, 2)) == "three");
  }
}
