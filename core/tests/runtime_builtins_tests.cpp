#include <catch2/catch_test_macros.hpp>

#include "fleaux/runtime/fleaux_runtime.hpp"

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
    Value len = make_tuple(seq) | Length{};
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

  Value r = make_tuple(make_bool(true), make_int(11), make_int(22)) | Select{};
  REQUIRE(to_double(r) == 11.0);
}

TEST_CASE("Runtime builtins: loop and formatting", "[runtime]") {
  SECTION("Loop sums 1..5") {
    auto cf = [](Value state) -> Value {
      return make_bool(to_double(array_at(state, 0)) > 0.0);
    };
    auto sf = [](Value state) -> Value {
      const double n = to_double(array_at(state, 0));
      const double acc = to_double(array_at(state, 1));
      return make_tuple(make_int(static_cast<Int>(n - 1)),
                        make_int(static_cast<Int>(acc + n)));
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
    REQUIRE(as_string(make_tuple(text, make_string("[,;]"), make_string("|")) | StringRegexReplace{}) == "one|two|three");

    const Value parts = make_tuple(text, make_string("[,;]")) | StringRegexSplit{};
    REQUIRE(as_array(parts).Size() == 3);
    REQUIRE(as_string(array_at(parts, 0)) == "one");
    REQUIRE(as_string(array_at(parts, 1)) == "two");
    REQUIRE(as_string(array_at(parts, 2)) == "three");
  }
}

