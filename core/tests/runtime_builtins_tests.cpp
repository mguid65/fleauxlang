#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    REQUIRE_THROWS_WITH(make_tuple(tpl, make_float(1.9)) | ElementAt{},
                        Catch::Matchers::ContainsSubstring("expects an integer value"));
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
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(2)) | Add{}) | Type{}) == "UInt64");
  REQUIRE_THROWS_AS(make_tuple(make_int(-2), make_uint(5)) | Add{}, std::invalid_argument);
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(2)) | Divide{}) | Type{}) == "UInt64");
  REQUIRE_THROWS_AS(make_tuple(make_int(-10), make_uint(3)) | Divide{}, std::invalid_argument);
  REQUIRE(as_string((make_tuple(make_uint(40), make_uint(3)) | Mod{}) | Type{}) == "UInt64");
  REQUIRE_THROWS_AS(make_tuple(make_int(-10), make_uint(3)) | Mod{}, std::invalid_argument);
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
  REQUIRE_THROWS_WITH(make_tuple(make_float(6.5), make_int(3)) | BitAnd{},
                      Catch::Matchers::ContainsSubstring("expects an integer value"));

  Value r = make_tuple(make_bool(true), make_int(11), make_int(22)) | Select{};
  REQUIRE(to_double(r) == 11.0);
}

TEST_CASE("Runtime builtins: arithmetic and bitwise boundaries", "[runtime][boundary]") {
  SECTION("Mixed signed/unsigned integer kinds are rejected consistently") {
    REQUIRE_THROWS_AS(make_tuple(make_int(-5), make_uint(2)) | Subtract{}, std::invalid_argument);
    REQUIRE_THROWS_AS(make_tuple(make_int(-5), make_uint(2)) | Multiply{}, std::invalid_argument);
    REQUIRE_THROWS_AS(make_tuple(make_int(-5), make_uint(2)) | Mod{}, std::invalid_argument);
  }

  SECTION("Bit shifts reject negative and non-integer shift values") {
    REQUIRE_THROWS_WITH(make_tuple(make_int(1), make_int(-1)) | BitShiftLeft{},
                        Catch::Matchers::ContainsSubstring("non-negative"));
    REQUIRE_THROWS_WITH(make_tuple(make_int(8), make_int(-1)) | BitShiftRight{},
                        Catch::Matchers::ContainsSubstring("non-negative"));
    REQUIRE_THROWS_WITH(make_tuple(make_int(1), make_float(1.5)) | BitShiftLeft{},
                        Catch::Matchers::ContainsSubstring("expects an integer value"));
    REQUIRE_THROWS_WITH(make_tuple(make_int(8), make_float(1.5)) | BitShiftRight{},
                        Catch::Matchers::ContainsSubstring("expects an integer value"));
  }
}

TEST_CASE("Runtime builtins: OS exec", "[runtime]") {
  const Value result = make_string("printf 'fleaux_exec_ok'") | OSExec{};
  REQUIRE(as_array(result).Size() == 2);
  REQUIRE(to_double(array_at(result, 0)) == 0.0);
  REQUIRE(as_string(array_at(result, 1)) == "fleaux_exec_ok");
}

TEST_CASE("Runtime builtins: FileReadChunk rejects fractional nbytes", "[runtime]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_runtime_builtins_tests";
  std::filesystem::create_directories(temp_dir);
  const auto file_path = temp_dir / "readchunk_numeric_guard.txt";
  {
    std::ofstream out(file_path);
    out << "abc";
  }

  const Value handle = make_tuple(make_string(file_path.string())) | FileOpen{};
  REQUIRE_THROWS_WITH(make_tuple(handle, make_float(1.5)) | FileReadChunk{},
                      Catch::Matchers::ContainsSubstring("expects an integer value"));

  (void)(handle | FileClose{});
  std::error_code ec;
  std::filesystem::remove(file_path, ec);
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

TEST_CASE("Runtime builtins: Std.Parallel.Map", "[runtime]") {
  SECTION("Parallel returns Ok with ordered mapped outputs") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one) | ParallelMap{};
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
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), fail_on_two) | ParallelMap{};
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
      const Value result = make_tuple(input, add_one) | ParallelMap{};
      REQUIRE(as_bool(result | ResultIsOk{}));
      const Value payload = result | ResultUnwrap{};
      REQUIRE(as_array(payload).Size() == 8);
      REQUIRE(to_double(array_at(payload, 0)) == 2.0);
      REQUIRE(to_double(array_at(payload, 7)) == 9.0);
    }
  }

  SECTION("Parallel stress does not grow callable registry beyond seeded refs") {
    reset_callable_registry();
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const auto seeded_size = callable_registry_size();
    REQUIRE(seeded_size == 1U);

    const Value input = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4));
    for (int iter = 0; iter < 120; ++iter) {
      const Value result = make_tuple(input, add_one) | ParallelMap{};
      REQUIRE(as_bool(result | ResultIsOk{}));
      REQUIRE(callable_registry_size() == seeded_size);
    }
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.WithOptions", "[runtime]") {
  SECTION("WithOptions delegates to Parallel.Map baseline") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value options{Object{}};
    as_object(options)["s:max_workers"] = make_int(4);

    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), add_one, options) | ParallelWithOptions{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    const Value payload = result | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 3);
    REQUIRE(to_double(array_at(payload, 0)) == 2.0);
    REQUIRE(to_double(array_at(payload, 2)) == 4.0);
  }

  SECTION("WithOptions returns Err when options is not a dict") {
    const Value identity = make_callable_ref([](const Value& v) -> Value { return v; });
    const Value result = make_tuple(make_tuple(make_int(1)), identity, make_int(5)) | ParallelWithOptions{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 0.0);
    REQUIRE(as_string(array_at(err, 1)) == "Parallel.WithOptions: options must be a Dict");
  }

  SECTION("WithOptions preserves deterministic output order under chunking") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });

    Value options{Object{}};
    as_object(options)["s:max_workers"] = make_int(2);

    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5)), add_one, options) |
        ParallelWithOptions{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    const Value payload = result | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 5);
    REQUIRE(to_double(array_at(payload, 0)) == 2.0);
    REQUIRE(to_double(array_at(payload, 1)) == 3.0);
    REQUIRE(to_double(array_at(payload, 2)) == 4.0);
    REQUIRE(to_double(array_at(payload, 3)) == 5.0);
    REQUIRE(to_double(array_at(payload, 4)) == 6.0);
  }

  SECTION("WithOptions reports global failing index across chunk boundaries") {
    const Value fail_on_four = make_callable_ref([](const Value& v) -> Value {
      if (as_int_value(v) == 4) { throw std::runtime_error("boom-four"); }
      return v;
    });

    Value options{Object{}};
    as_object(options)["s:max_workers"] = make_int(2);

    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5)), fail_on_four, options) |
        ParallelWithOptions{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 3.0);
    REQUIRE(as_string(array_at(err, 1)) == "boom-four");
  }

  SECTION("WithOptions rejects non-positive max_workers") {
    const Value identity = make_callable_ref([](const Value& v) -> Value { return v; });

    Value bad_workers{Object{}};
    as_object(bad_workers)["s:max_workers"] = make_int(0);
    const Value workers_result = make_tuple(make_tuple(make_int(1)), identity, bad_workers) | ParallelWithOptions{};
    REQUIRE(as_bool(workers_result | ResultIsErr{}));
    const Value err = workers_result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 0.0);
    REQUIRE(as_string(array_at(err, 1)) == "Parallel.WithOptions: max_workers must be > 0");
  }

  SECTION("WithOptions rejects unsupported option keys") {
    const Value identity = make_callable_ref([](const Value& v) -> Value { return v; });

    Value options{Object{}};
    as_object(options)["s:chunk_size"] = make_int(2);
    const Value result = make_tuple(make_tuple(make_int(1)), identity, options) | ParallelWithOptions{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 0.0);
    REQUIRE(as_string(array_at(err, 1)) == "Parallel.WithOptions: unsupported option 'chunk_size'");
  }
}

TEST_CASE("Runtime builtins: Std.Task.* queued backend", "[runtime]") {
  TaskRegistryScope task_scope;

  SECTION("Task.Spawn + Task.Await returns Ok(result)") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value task = make_tuple(add_one, make_int(41)) | TaskSpawn{};
    const Value awaited = make_tuple(task) | TaskAwait{};
    REQUIRE(as_bool(awaited | ResultIsOk{}));
    REQUIRE(to_double(awaited | ResultUnwrap{}) == 42.0);
  }

  SECTION("Task.AwaitAll collects payloads and preserves order") {
    const Value add_one = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const Value t1 = make_tuple(add_one, make_int(1)) | TaskSpawn{};
    const Value t2 = make_tuple(add_one, make_int(2)) | TaskSpawn{};
    const Value t3 = make_tuple(add_one, make_int(3)) | TaskSpawn{};

    const Value all = make_tuple(make_tuple(t1, t2, t3)) | TaskAwaitAll{};
    REQUIRE(as_bool(all | ResultIsOk{}));
    const Value payload = all | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 3);
    REQUIRE(to_double(array_at(payload, 0)) == 2.0);
    REQUIRE(to_double(array_at(payload, 1)) == 3.0);
    REQUIRE(to_double(array_at(payload, 2)) == 4.0);
  }

  SECTION("Task.AwaitAll returns Err(index, message) on failing task") {
    const Value maybe_fail = make_callable_ref([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("boom-two"); }
      return v;
    });
    const Value t1 = make_tuple(maybe_fail, make_int(1)) | TaskSpawn{};
    const Value t2 = make_tuple(maybe_fail, make_int(2)) | TaskSpawn{};
    const Value all = make_tuple(make_tuple(t1, t2)) | TaskAwaitAll{};

    REQUIRE(as_bool(all | ResultIsErr{}));
    const Value err = all | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 1.0);
    REQUIRE(as_string(array_at(err, 1)) == "boom-two");
  }

  SECTION("Task.Cancel can cancel a queued task before execution") {
    using namespace std::chrono_literals;

    const Value slow_identity = make_callable_ref([](const Value& v) -> Value {
      std::this_thread::sleep_for(50ms);
      return v;
    });
    const Value fast_identity = make_callable_ref([](const Value& v) -> Value { return v; });

    const Value first = make_tuple(slow_identity, make_int(7)) | TaskSpawn{};
    const Value second = make_tuple(fast_identity, make_int(8)) | TaskSpawn{};

    const Value cancel_result = make_tuple(second) | TaskCancel{};
    const bool cancelled = as_bool(cancel_result);

    const Value first_result = make_tuple(first) | TaskAwait{};
    REQUIRE(as_bool(first_result | ResultIsOk{}));
    REQUIRE(to_double(first_result | ResultUnwrap{}) == 7.0);

    const Value second_result = make_tuple(second) | TaskAwait{};
    if (cancelled) {
      REQUIRE(as_bool(second_result | ResultIsErr{}));
      REQUIRE(as_string(second_result | ResultUnwrapErr{}) == "Task cancelled");
    } else {
      // Multi-worker scheduling can start the second task before cancel is processed.
      REQUIRE(as_bool(second_result | ResultIsOk{}));
      REQUIRE(to_double(second_result | ResultUnwrap{}) == 8.0);
    }
  }

  SECTION("Task.WithTimeout reports timeout and later await still succeeds") {
    using namespace std::chrono_literals;

    const Value slow_identity = make_callable_ref([](const Value& v) -> Value {
      std::this_thread::sleep_for(40ms);
      return v;
    });
    const Value task = make_tuple(slow_identity, make_int(7)) | TaskSpawn{};

    const Value timed = make_tuple(task, make_int(1)) | TaskWithTimeout{};
    REQUIRE(as_bool(timed | ResultIsErr{}));
    REQUIRE(as_string(timed | ResultUnwrapErr{}) == "Task.WithTimeout: timeout exceeded");

    const Value awaited = make_tuple(task) | TaskAwait{};
    REQUIRE(as_bool(awaited | ResultIsOk{}));
    REQUIRE(to_double(awaited | ResultUnwrap{}) == 7.0);
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.ForEach", "[runtime]") {
  SECTION("ForEach returns Ok(empty tuple) when all items succeed") {
    std::atomic<int> counter{0};
    const Value bump = make_callable_ref([&counter](const Value& /*v*/) -> Value {
      counter.fetch_add(1, std::memory_order_relaxed);
      return make_bool(true);
    });
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), bump) | ParallelForEach{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    // Output is empty tuple.
    const Value payload = result | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 0);
    // All items were visited.
    REQUIRE(counter.load() == 3);
  }

  SECTION("ForEach returns Err(index, message) when an item fails") {
    const Value fail_on_two = make_callable_ref([](const Value& v) -> Value {
      if (as_int_value(v) == 2) { throw std::runtime_error("fe-error"); }
      return make_bool(true);
    });
    const Value result = make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), fail_on_two) | ParallelForEach{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(as_string(array_at(err, 1)) == "fe-error");
  }

  SECTION("ForEach on empty input returns Ok(empty tuple)") {
    const Value noop = make_callable_ref([](const Value& v) -> Value { return v; });
    const Value result = make_tuple(make_tuple(), noop) | ParallelForEach{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(as_array(result | ResultUnwrap{}).Size() == 0);
  }

  SECTION("ForEach stress: large input all items visited in parallel") {
    constexpr int n = 500;
    std::atomic<int> counter{0};
    const Value bump = make_callable_ref([&counter](const Value& /*v*/) -> Value {
      counter.fetch_add(1, std::memory_order_relaxed);
      return make_bool(true);
    });
    Array items;
    items.Reserve(n);
    for (int i = 0; i < n; ++i) { items.PushBack(make_int(i)); }
    const Value result = make_tuple(Value{std::move(items)}, bump) | ParallelForEach{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(counter.load() == n);
  }
}

TEST_CASE("Runtime builtins: Std.Parallel.Reduce", "[runtime]") {
  SECTION("Reduce sums a small list") {
    const Value add = make_callable_ref([](const Value& arg) -> Value {
      const auto& arr = as_array(arg);
      const Int a = as_int_value(*arr.TryGet(0));
      const Int b = as_int_value(*arr.TryGet(1));
      return make_int(a + b);
    });
    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4)), make_int(0), add) | ParallelReduce{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(to_double(result | ResultUnwrap{}) == 10.0);
  }

  SECTION("Reduce with plain (non-Result) return value works") {
    const Value add = make_callable_ref([](const Value& arg) -> Value {
      const auto& arr = as_array(arg);
      return make_int(as_int_value(*arr.TryGet(0)) + as_int_value(*arr.TryGet(1)));
    });
    const Value result =
        make_tuple(make_tuple(make_int(10), make_int(20), make_int(30)), make_int(0), add) | ParallelReduce{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(to_double(result | ResultUnwrap{}) == 60.0);
  }

  SECTION("Reduce on empty input returns Ok(init)") {
    const Value add = make_callable_ref([](const Value& arg) -> Value { return arg; });
    const Value result = make_tuple(make_tuple(), make_int(99), add) | ParallelReduce{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    REQUIRE(to_double(result | ResultUnwrap{}) == 99.0);
  }

  SECTION("Reduce preserves accumulator tuples that begin with Bool") {
    const Value fold = make_callable_ref([](const Value& arg) -> Value {
      const auto& arr = as_array(arg);
      const auto& acc = as_array(*arr.TryGet(0));
      const bool flag = as_bool(*acc.TryGet(0));
      const Int sum = as_int_value(*acc.TryGet(1)) + as_int_value(*arr.TryGet(1));
      return make_tuple(make_bool(flag), make_int(sum));
    });
    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3)), make_tuple(make_bool(false), make_int(0)), fold) |
        ParallelReduce{};
    REQUIRE(as_bool(result | ResultIsOk{}));
    const Value payload = result | ResultUnwrap{};
    REQUIRE(as_array(payload).Size() == 2);
    REQUIRE(as_bool(array_at(payload, 0)) == false);
    REQUIRE(to_double(array_at(payload, 1)) == 6.0);
  }

  SECTION("Reduce annotates thrown step exceptions with global index") {
    const Value fail = make_callable_ref([](const Value& arg) -> Value {
      const auto& arr = as_array(arg);
      const Int b = as_int_value(*arr.TryGet(1));
      if (b == 3) { throw std::runtime_error("reduce-throw"); }
      return *arr.TryGet(0);
    });
    const Value result =
        make_tuple(make_tuple(make_int(1), make_int(2), make_int(3), make_int(4)), make_int(0), fail) |
        ParallelReduce{};
    REQUIRE(as_bool(result | ResultIsErr{}));
    const Value err = result | ResultUnwrapErr{};
    REQUIRE(as_array(err).Size() == 2);
    REQUIRE(to_double(array_at(err, 0)) == 2.0);
    REQUIRE(as_string(array_at(err, 1)) == "reduce-throw");
  }
}

TEST_CASE("Runtime builtins: concurrency property tests", "[runtime][concurrency]") {
  SECTION("Parallel.Map is semantically equivalent to sequential map for pure functions") {
    // Property: for a pure function f and any input list,
    // Parallel.Map(items, f) == [f(items[0]), f(items[1]), ...]
    const Value double_val = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) * 2); });

    constexpr int runs = 100;
    for (int run = 0; run < runs; ++run) {
      const int n = (run % 16) + 1;
      Array items;
      items.Reserve(n);
      for (int i = 0; i < n; ++i) { items.PushBack(make_int(i + 1)); }

      const Value result = make_tuple(Value{items}, double_val) | ParallelMap{};
      REQUIRE(as_bool(result | ResultIsOk{}));
      const Value payload = result | ResultUnwrap{};
      REQUIRE(as_array(payload).Size() == static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i) {
        REQUIRE(to_double(array_at(payload, static_cast<std::size_t>(i))) == static_cast<double>((i + 1) * 2));
      }
    }
  }

  SECTION("Parallel.Map output order is always input order under concurrent execution") {
    // Use a function that sleeps a small random amount to trigger scheduling variance.
    const Value delayed = make_callable_ref([](const Value& v) -> Value {
      const int ms = static_cast<int>(as_int_value(v)) % 5;
      if (ms > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
      return v;
    });

    constexpr int items_count = 20;
    Array items;
    items.Reserve(items_count);
    for (int i = 0; i < items_count; ++i) { items.PushBack(make_int(i)); }

    for (int run = 0; run < 20; ++run) {
      const Value result = make_tuple(Value{items}, delayed) | ParallelMap{};
      REQUIRE(as_bool(result | ResultIsOk{}));
      const Value payload = result | ResultUnwrap{};
      REQUIRE(as_array(payload).Size() == items_count);
      for (int i = 0; i < items_count; ++i) {
        REQUIRE(to_double(array_at(payload, static_cast<std::size_t>(i))) == static_cast<double>(i));
      }
    }
  }

  SECTION("Parallel.Map callable registry does not grow under repeated concurrent use") {
    reset_callable_registry();
    const Value fn = make_callable_ref([](const Value& v) -> Value { return v; });
    const auto baseline = callable_registry_size();

    const Value items = make_tuple(make_int(1), make_int(2), make_int(3), make_int(4), make_int(5), make_int(6),
                                   make_int(7), make_int(8));
    for (int i = 0; i < 150; ++i) {
      const Value result = make_tuple(items, fn) | ParallelMap{};
      REQUIRE(as_bool(result | ResultIsOk{}));
    }
    REQUIRE(callable_registry_size() == baseline);
  }

  SECTION("Concurrent Task.Spawn + Parallel.Map interleaved: registries stable") {
    reset_callable_registry();
    reset_value_registry_for_tests();
    reset_task_registry_for_tests();
    const Value fn = make_callable_ref([](const Value& v) -> Value { return make_int(as_int_value(v) + 1); });
    const auto baseline = callable_registry_size();
    auto telemetry = value_registry_telemetry();
    REQUIRE(telemetry.active_count == 0U);
    REQUIRE(telemetry.rejected_allocations == 0U);
    REQUIRE(telemetry.stale_deref_rejections == 0U);
    REQUIRE_FALSE(telemetry.transient_cap.has_value());
    REQUIRE(task_registry_size() == 0U);

    std::size_t prev_peak = telemetry.peak_active_count;
    {
      TaskRegistryScope task_scope;
      for (int i = 0; i < 30; ++i) {
        const Value task = make_tuple(fn, make_int(i)) | TaskSpawn{};
        const Value items = make_tuple(make_int(i), make_int(i + 1), make_int(i + 2));
        const Value map_result = make_tuple(items, fn) | ParallelMap{};
        REQUIRE(as_bool(map_result | ResultIsOk{}));
        const Value awaited = make_tuple(task) | TaskAwait{};
        REQUIRE(as_bool(awaited | ResultIsOk{}));

        const auto iter_telemetry = value_registry_telemetry();
        REQUIRE(iter_telemetry.active_count == 0U);
        REQUIRE(iter_telemetry.rejected_allocations == 0U);
        REQUIRE(iter_telemetry.stale_deref_rejections == 0U);
        REQUIRE_FALSE(iter_telemetry.transient_cap.has_value());
        REQUIRE(iter_telemetry.peak_active_count >= prev_peak);
        prev_peak = iter_telemetry.peak_active_count;
      }
    }

    REQUIRE(callable_registry_size() == baseline);
    const auto final_telemetry = value_registry_telemetry();
    REQUIRE(final_telemetry.active_count == 0U);
    REQUIRE(final_telemetry.rejected_allocations == 0U);
    REQUIRE(final_telemetry.stale_deref_rejections == 0U);
    REQUIRE_FALSE(final_telemetry.transient_cap.has_value());
    REQUIRE(final_telemetry.peak_active_count >= prev_peak);
    REQUIRE(task_registry_size() == 0U);
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

  SECTION("Printf returns original arg tuple without newline") {
    StdoutCapture capture;
    const Value result = make_tuple(make_string("{} + {}"), make_int(1), make_int(2)) | Printf{};
    REQUIRE(capture.str() == "1 + 2");

    const auto& returned = as_array(result);
    REQUIRE(returned.Size() == 3U);
    REQUIRE(as_string(*returned.TryGet(0)) == "{} + {}");
    REQUIRE(to_double(*returned.TryGet(1)) == 1.0);
    REQUIRE(to_double(*returned.TryGet(2)) == 2.0);
  }
}

TEST_CASE("Runtime builtins: Std.Type", "[runtime]") {
  REQUIRE(as_string(make_int(42) | Type{}) == "Int64");
  REQUIRE(as_string(make_uint(42) | Type{}) == "UInt64");
  REQUIRE(as_string(make_float(3.14) | Type{}) == "Float64");
  REQUIRE(as_string(make_bool(true) | Type{}) == "Bool");
  REQUIRE(as_string(make_string("x") | Type{}) == "String");
  REQUIRE(as_string(make_null() | Type{}) == "Null");
  REQUIRE(as_string(make_tuple(make_int(1), make_int(2)) | Type{}) == "Tuple");

  Value dict{Object{}};
  REQUIRE(as_string(dict | Type{}) == "Dict");

  const Value callable = make_callable_ref([](const Value& v) -> Value { return v; });
  REQUIRE(as_string(callable | Type{}) == "Callable");
}

TEST_CASE("Runtime builtins: tuple min/max boundaries", "[runtime][boundary]") {
  SECTION("TupleMin and TupleMax reject empty tuples") {
    REQUIRE_THROWS_WITH(make_tuple() | TupleMin{}, Catch::Matchers::ContainsSubstring("non-empty"));
    REQUIRE_THROWS_WITH(make_tuple() | TupleMax{}, Catch::Matchers::ContainsSubstring("non-empty"));
  }

  SECTION("TupleMin and TupleMax handle singleton tuples") {
    REQUIRE(to_double(make_tuple(make_int(7)) | TupleMin{}) == 7.0);
    REQUIRE(to_double(make_tuple(make_int(7)) | TupleMax{}) == 7.0);
  }

  SECTION("TupleMin and TupleMax choose boundaries across numeric range") {
    const Value values = make_tuple(make_int(-9223372036854775807LL), make_int(0), make_int(9223372036854775807LL));
    REQUIRE(to_double(values | TupleMin{}) < -9.0e18);
    REQUIRE(to_double(values | TupleMax{}) > 9.0e18);
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

    REQUIRE_THROWS_WITH(make_tuple(cube, make_tuple(make_int(1), make_float(0.5), make_int(1))) | ArrayGetAtND{},
                        Catch::Matchers::ContainsSubstring("expects an integer value"));
    REQUIRE_THROWS_WITH(make_tuple(flattened, make_tuple(make_int(2), make_float(2.5), make_int(2))) | ArrayReshapeND{},
                        Catch::Matchers::ContainsSubstring("expects an integer value"));

    const Value whole = make_tuple(cube, make_tuple()) | ArrayGetAtND{};
    REQUIRE(as_array(whole).Size() == 2);

    REQUIRE_THROWS_WITH(make_tuple(cube, make_tuple(make_int(-1), make_int(0), make_int(0))) | ArrayGetAtND{},
                        Catch::Matchers::ContainsSubstring("non-negative index"));
    REQUIRE_THROWS_AS(make_tuple(cube, make_tuple(make_int(0), make_int(0), make_int(0), make_int(0))) | ArrayGetAtND{},
                      std::runtime_error);

    REQUIRE_THROWS_WITH(make_tuple(flattened, make_tuple(make_int(2), make_int(2), make_int(3))) | ArrayReshapeND{},
                        Catch::Matchers::ContainsSubstring("shape product"));

    REQUIRE_THROWS_WITH(make_tuple(make_tuple(), make_tuple()) | ArrayReshapeND{},
                        Catch::Matchers::ContainsSubstring("shape product"));

    const Value scalar_reshape = make_tuple(make_tuple(make_int(9)), make_tuple()) | ArrayReshapeND{};
    REQUIRE(to_double(scalar_reshape) == 9.0);
  }
}

TEST_CASE("Runtime builtins: temp lifecycle and file handle cleanup", "[runtime][boundary]") {
  SECTION("OSMakeTempFile and OSMakeTempDir materialize resources") {
    const Value temp_file = make_tuple() | OSMakeTempFile{};
    const Value temp_dir = make_tuple() | OSMakeTempDir{};

    REQUIRE(temp_file.HasString());
    REQUIRE(temp_dir.HasString());

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
    const Value capture_and_return = make_callable_ref([&captured_token](const Value& token) -> Value {
      captured_token = token;
      return make_string("ok");
    });

    const Value result =
        make_tuple(make_string(file_path.string()), make_string("r"), capture_and_return) | FileWithOpen{};
    REQUIRE(as_string(result) == "ok");
    REQUIRE_THROWS_WITH(captured_token | FileReadLine{}, Catch::Matchers::ContainsSubstring("closed or invalid"));

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
    const Value capture_and_throw = make_callable_ref([&captured_token](const Value& token) -> Value {
      captured_token = token;
      throw std::runtime_error("boom");
    });

    REQUIRE_THROWS_WITH(
        make_tuple(make_string(file_path.string()), make_string("r"), capture_and_throw) | FileWithOpen{},
        Catch::Matchers::ContainsSubstring("boom"));
    REQUIRE_THROWS_WITH(captured_token | FileReadLine{}, Catch::Matchers::ContainsSubstring("closed or invalid"));

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

    const Value first_token = make_tuple(make_string(file_path.string())) | FileOpen{};
    REQUIRE(as_bool(first_token | FileClose{}));
    REQUIRE_FALSE(as_bool(first_token | FileClose{}));
    REQUIRE_THROWS_WITH(first_token | FileReadLine{}, Catch::Matchers::ContainsSubstring("closed or invalid"));

    const Value reopened_token = make_tuple(make_string(file_path.string())) | FileOpen{};
    const Value read_back = reopened_token | FileReadLine{};
    REQUIRE(as_array(read_back).Size() == 3);
    REQUIRE(as_string(array_at(read_back, 1)) == "line");
    REQUIRE(as_bool(reopened_token | FileClose{}));

    std::error_code ec;
    std::filesystem::remove(file_path, ec);
    std::filesystem::remove_all(temp_dir, ec);
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

TEST_CASE("Runtime builtins: numeric cast helpers", "[runtime]") {
  SECTION("ToInt64") {
    REQUIRE(as_int_value(make_int(42) | ToInt64{}) == 42);
    REQUIRE(as_int_value(make_float(3.0) | ToInt64{}) == 3);
    REQUIRE_THROWS_AS(make_float(1.5) | ToInt64{}, std::invalid_argument);
  }

  SECTION("ToUInt64") {
    REQUIRE(is_uint_number(make_int(5) | ToUInt64{}));
    REQUIRE(is_uint_number(make_uint(10) | ToUInt64{}));
    REQUIRE_THROWS_AS(make_int(-1) | ToUInt64{}, std::invalid_argument);
    REQUIRE_THROWS_AS(make_float(-0.5) | ToUInt64{}, std::invalid_argument);
  }

  SECTION("ToFloat64") {
    REQUIRE(is_float_number(make_int(7) | ToFloat64{}));
    REQUIRE(is_float_number(make_uint(7) | ToFloat64{}));
    REQUIRE(is_float_number(make_float(7.0) | ToFloat64{}));
    REQUIRE(to_double(make_int(3) | ToFloat64{}) == 3.0);
  }
}
