// The calls of a failed run: which functions it failed in, where, and how diagnostics show them.

#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/vm/runtime.hpp"
#include "vm_test_support.hpp"

namespace {

using fleaux::vm::RuntimeCall;
using fleaux::vm::RuntimeError;

// Writes `files` into a directory of their own and runs `entry`, which must fail.
auto failure_of(const std::string& case_name, const std::map<std::string, std::string>& files, const std::string& entry) -> RuntimeError {
  const auto directory = std::filesystem::temp_directory_path() / "fleaux_runtime_calls_tests" / case_name;
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  for (const auto& [name, source] : files) {
    fleaux::tests::write_text_file(directory / name, source);
  }
  const auto loaded =
      fleaux::bytecode::load_linked_module(directory / entry, fleaux::bytecode::ModuleLoadOptions{.write_bytecode_cache = false});
  if (!loaded) {
    INFO("load error: " << loaded.error().message);
  }
  REQUIRE(loaded.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*loaded, output);
  REQUIRE_FALSE(executed.has_value());
  return executed.error();
}

auto functions_of(const std::vector<RuntimeCall>& calls) -> std::vector<std::string> {
  std::vector<std::string> functions;
  for (const auto& call : calls) {
    functions.push_back(call.function);
  }
  return functions;
}

auto call_at(const std::string& function, const std::uint32_t line, const std::uint32_t column, const std::string& through = {})
    -> RuntimeCall {
  return RuntimeCall{.function = function, .through = through, .file = "/src/main.fleaux", .line = line, .column = column};
}

}  // namespace

TEST_CASE("A failure names the functions it happened in, innermost first", "[runtime][calls]") {
  const auto error = failure_of("functions", {{"main.fleaux", R"(import Std;
let Inner(x: Int64): Int64 = (10, x) -> Std.Divide -> (_, 1) -> Std.Add;
let Outer(t: Tuple<Int64...>): Tuple<Int64...> = (t, Inner) -> Std.Tuple.Map;
((5, 0)) -> Outer -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(error.message == "Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{"Inner", "Outer", ""});
  REQUIRE(error.calls[0].line == 2U);
  REQUIRE(error.calls[1].line == 3U);
  REQUIRE(error.calls[2].line == 4U);
  REQUIRE(std::filesystem::path(error.calls[0].file).filename() == "main.fleaux");
}

TEST_CASE("A function a builtin calls back continues the chain through the builtin", "[runtime][calls]") {
  const auto error = failure_of("through", {{"main.fleaux", R"(import Std;
let Ratio(x: Int64): Int64 = (10, x) -> Std.Divide;
let Large(x: Int64): Bool = ((x) -> Ratio, 2) -> Std.GreaterThan;
((5, 0, 1), Large) -> Std.Tuple.Any -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(error.message == "Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{"Ratio", "Large", ""});
  REQUIRE(error.calls[1].through.empty());
  REQUIRE(error.calls[2].through == "Std.Tuple.Any");
  REQUIRE(error.calls[2].line == 4U);
  REQUIRE(error.calls[2].column == 23U);
}

TEST_CASE("A chain through two builtins that call back keeps every call in order", "[runtime][calls]") {
  // Std.Tuple.Any calls Large, which calls Std.Tuple.SortBy, which calls Key: two scratch loops, each adding its calls
  // when it fails, inside the top level's.
  const auto error = failure_of("nested_through", {{"main.fleaux", R"(import Std;
let Key(x: Int64): Int64 = (10, x) -> Std.Divide;
let Large(x: Int64): Bool = ((x, 0), Key) -> Std.Tuple.SortBy -> Std.Length -> (_, 1) -> Std.GreaterThan;
((5, 0, 1), Large) -> Std.Tuple.Any -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(error.message == "Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{"Key", "Large", ""});
  REQUIRE(error.calls[0].line == 2U);
  REQUIRE(error.calls[1].through == "Std.Tuple.SortBy");
  REQUIRE(error.calls[1].line == 3U);
  REQUIRE(error.calls[2].through == "Std.Tuple.Any");
  REQUIRE(error.calls[2].line == 4U);
}

TEST_CASE("A function a builtin runs in place is called by the builtin", "[runtime][calls][in_place]") {
  // Each builtin with an in-place form, with a named function and with an inline closure: the chain is the one the
  // builtin's call of the function from C++ gives.
  struct Case {
    std::string builtin;
    std::string call;
    std::string function;
  };
  const std::vector<Case> cases{
      {"Std.Tuple.Map", "((0,), Bad) -> Std.Tuple.Map", "Bad"},
      {"Std.Tuple.Map", "((0,), (x: Int64): Int64 = (10, x) -> Std.Divide) -> Std.Tuple.Map", "a closure"},
      {"Std.Tuple.Filter", "((0,), (x: Int64): Bool = ((10, x) -> Std.Divide, 0) -> Std.GreaterThan) -> Std.Tuple.Filter", "a closure"},
      {"Std.Tuple.Reduce", "((0,), 0, (a: Int64, x: Int64): Int64 = (a, (10, x) -> Std.Divide) -> Std.Add) -> Std.Tuple.Reduce",
       "a closure"},
      {"Std.Branch", "(True, 0, Bad, Bad) -> Std.Branch", "Bad"},
      {"Std.Branch", "(True, 0, (x: Int64): Int64 = (10, x) -> Std.Divide, Bad) -> Std.Branch", "a closure"},
      {"Std.Loop", "(0, Keep, Bad) -> Std.Loop", "Bad"},
      {"Std.Loop", "(0, Keep, (x: Int64): Int64 = (10, x) -> Std.Divide) -> Std.Loop", "a closure"},
      {"Std.Match", "(0, (0, Bad), (_, Bad)) -> Std.Match", "Bad"},
      {"Std.Match", "(0, (0, (): Int64 = (10, 0) -> Std.Divide), (_, Bad)) -> Std.Match", "a closure"},
      {"Std.Apply", "(0, Bad) -> Std.Apply", "Bad"},
      {"Std.Apply", "(0, (x: Int64): Int64 = (10, x) -> Std.Divide) -> Std.Apply", "a closure"},
  };
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const Case& in_place = cases[index];
    INFO(in_place.call);
    const auto error = failure_of("in_place_" + std::to_string(index),
                                  {{"main.fleaux",
                                    "import Std;\n"
                                    "let Bad(x: Int64): Int64 = (10, x) -> Std.Divide;\n"
                                    "let Keep(x: Int64): Bool = True;\n" +
                                        in_place.call + " -> Std.Println;\n"}},
                                  "main.fleaux");
    REQUIRE(error.message == "Std.Divide: Int64 division by zero");
    REQUIRE(functions_of(error.calls) == std::vector<std::string>{in_place.function, ""});
    REQUIRE(error.calls[1].through == in_place.builtin);
    REQUIRE(error.calls[1].line == 4U);
  }
}

TEST_CASE("A failure in an imported module names that module's file", "[runtime][calls][import]") {
  const auto error = failure_of("import",
                                {{"lib.fleaux", R"(import Std;
let Ratio(x: Int64): Int64 = (10, x) -> Std.Divide;
)"},
                                 {"main.fleaux", R"(import Std;
import lib;
(0) -> Ratio -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(error.message == "Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{"Ratio", ""});
  REQUIRE(std::filesystem::path(error.calls[0].file).filename() == "lib.fleaux");
  REQUIRE(error.calls[0].line == 2U);
  REQUIRE(std::filesystem::path(error.calls[1].file).filename() == "main.fleaux");
  REQUIRE(error.calls[1].line == 3U);
}

TEST_CASE("A closure appears as a closure", "[runtime][calls]") {
  const auto error = failure_of("closure", {{"main.fleaux", R"(import Std;
let Divider(n: Int64): (Int64) => Int64 = (x: Int64): Int64 = (x, n) -> Std.Divide;
(7, (0) -> Divider) -> Std.Apply -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(error.message == "Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{"a closure", ""});
}

TEST_CASE("Std.Try's Err holds the message alone", "[runtime][calls][try]") {
  const auto error = failure_of("try", {{"main.fleaux", R"(import Std;
let Ratio(x: Int64): Int64 = (10, x) -> Std.Divide;
(0, Ratio) -> Std.Try -> Std.Result.Unwrap -> Std.Println;
)"}},
                                "main.fleaux");
  // The Err is the failure's message, and the failure's calls end at Std.Try.
  REQUIRE(error.message == "Std.Result.Unwrap: expected Ok, got Err: Std.Divide: Int64 division by zero");
  REQUIRE(functions_of(error.calls) == std::vector<std::string>{""});
}

TEST_CASE("The notes of a failure's calls", "[runtime][calls][diagnostics]") {
  SECTION("A failure at the top level has no notes") { REQUIRE(fleaux::vm::describe_runtime_calls({call_at("", 3U, 7U)}).empty()); }

  SECTION("Each call is a line, and a builtin that called back is named") {
    const auto notes =
        fleaux::vm::describe_runtime_calls({call_at("Inner", 2U, 40U), call_at("Pred", 3U, 36U), call_at("", 4U, 22U, "Std.Tuple.Any")});
    REQUIRE(notes == std::vector<std::string>{"in Inner", "called from Pred (main.fleaux:3:36)",
                                              "called by Std.Tuple.Any from the top level (main.fleaux:4:22)"});
  }

  SECTION("Consecutive calls from one place are one line") {
    const auto notes = fleaux::vm::describe_runtime_calls(
        {call_at("Count", 2U, 30U), call_at("Count", 2U, 50U), call_at("Count", 2U, 50U), call_at("Count", 2U, 50U), call_at("", 3U, 8U)});
    REQUIRE(notes == std::vector<std::string>{"in Count", "called from Count (main.fleaux:2:50), 3 times",
                                              "called from the top level (main.fleaux:3:8)"});
  }

  SECTION("A long chain keeps its first and last lines") {
    std::vector<RuntimeCall> calls{call_at("F0", 1U, 1U)};
    for (std::uint32_t index = 1U; index <= 30U; ++index) {
      calls.push_back(call_at("F" + std::to_string(index), index, 1U));
    }
    const auto notes = fleaux::vm::describe_runtime_calls(calls);
    REQUIRE(notes.size() == fleaux::vm::kRuntimeCallNoteLimit + 1U);
    REQUIRE(notes[0] == "in F0");
    REQUIRE(notes[14] == "called from F14 (main.fleaux:14:1)");
    REQUIRE(notes[15] == "... 11 more lines of calls");
    REQUIRE(notes[16] == "called from F26 (main.fleaux:26:1)");
    REQUIRE(notes[20] == "called from F30 (main.fleaux:30:1)");
  }

  SECTION("A call without a location says so") {
    REQUIRE(fleaux::vm::describe_runtime_calls({call_at("Inner", 2U, 4U), RuntimeCall{.function = "Outer"}}) ==
            std::vector<std::string>{"in Inner", "called from Outer (no location)"});
  }
}

TEST_CASE("The span of a failure shows its source line only while the file is unchanged", "[runtime][calls][diagnostics]") {
  const auto directory = std::filesystem::temp_directory_path() / "fleaux_runtime_calls_tests" / "span";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "main.fleaux";
  const std::string source = "import Std;\n(1, 0) -> Std.Divide -> Std.Println;\n";
  fleaux::tests::write_text_file(path, source);
  const auto loaded = fleaux::bytecode::load_linked_module(path, fleaux::bytecode::ModuleLoadOptions{.write_bytecode_cache = false});
  REQUIRE(loaded.has_value());
  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto executed = runtime.execute(*loaded, output);
  REQUIRE_FALSE(executed.has_value());

  const auto span = fleaux::vm::runtime_failure_span(executed.error());
  REQUIRE(span.has_value());
  REQUIRE(span->line == 2);
  REQUIRE(span->col == 11);
  REQUIRE(span->end_col == 21);
  // The text is the file's as read, whose line endings the platform may have written differently.
  const auto file_text = fleaux::frontend::source_loader::read_text_file(path);
  REQUIRE(file_text.has_value());
  REQUIRE(span->source_text() == *file_text);
  REQUIRE_THAT(fleaux::frontend::diag::format_diagnostic("vm-run", executed.error().message, span),
               Catch::Matchers::ContainsSubstring("2 | (1, 0) -> Std.Divide -> Std.Println;"));

  fleaux::tests::write_text_file(path, "import Std;\n\n(1, 0) -> Std.Divide -> Std.Println;\n");
  const auto stale = fleaux::vm::runtime_failure_span(executed.error());
  REQUIRE(stale.has_value());
  REQUIRE(stale->line == 2);
  REQUIRE(stale->source_text().empty());

  const auto from_reader = fleaux::vm::runtime_failure_span(
      executed.error(), [&file_text](const std::string&) -> std::optional<std::string> { return *file_text; });
  REQUIRE(from_reader.has_value());
  REQUIRE(from_reader->source_text() == *file_text);
}

TEST_CASE("Std.Panic, Std.Assert, and Std.AssertEqual stop the run with their message at their call", "[runtime][calls][panic]") {
  const auto panic = failure_of("panic", {{"main.fleaux", R"(import Std;
let Check(x: Int64): Int64 = ("x is too large") -> Std.Panic;
(3) -> Check -> Std.Println;
)"}},
                                "main.fleaux");
  REQUIRE(panic.message == "x is too large");
  REQUIRE(functions_of(panic.calls) == std::vector<std::string>{"Check", ""});
  REQUIRE(panic.calls[0].line == 2U);
  REQUIRE(panic.calls[0].column == 52U);

  const auto assertion = failure_of("assert", {{"main.fleaux", R"(import Std;
((1, 2) -> Std.LessThan, "one is less than two") -> Std.Assert -> Std.Println;
((2, 1) -> Std.LessThan, "two is less than one") -> Std.Assert -> Std.Println;
)"}},
                                    "main.fleaux");
  REQUIRE(assertion.message == "two is less than one");
  REQUIRE(assertion.calls[0].line == 3U);

  const auto equality = failure_of("assert_equal", {{"main.fleaux", R"(import Std;
((2, 2) -> Std.Add, 4) -> Std.AssertEqual -> Std.Println;
((2, 3) -> Std.Add, 4) -> Std.AssertEqual -> Std.Println;
)"}},
                                   "main.fleaux");
  REQUIRE(equality.message == "expected 4, got 5");
  REQUIRE(equality.calls[0].line == 3U);
}

TEST_CASE("The Result functions type-check and run in a program", "[runtime][result]") {
  const auto directory = std::filesystem::temp_directory_path() / "fleaux_runtime_calls_tests" / "result_functions";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  fleaux::tests::write_text_file(directory / "main.fleaux", R"(import Std;
let Double(x: Int64): Int64 = (x, 2) -> Std.Multiply;
let Half(x: Int64): Result<Int64, String> =
    ((x, 2) -> Std.Mod -> (_, 0) -> Std.Equal, (x, 2) -> Std.Divide -> Std.Result.Ok, ("odd") -> Std.Result.Err)
        -> Std.Select;
let Zero(error: String): Result<Int64, String> = (0) -> Std.Result.Ok;

("21") -> Std.String.ParseInt64 -> (_, Double) -> Std.Result.Map -> Std.Println;
("x") -> Std.String.ParseInt64 -> (_, Double) -> Std.Result.Map -> Std.Result.IsErr -> Std.Println;
("x") -> Std.String.ParseInt64 -> (_, Std.String.Length) -> Std.Result.MapErr -> Std.Result.UnwrapErr -> Std.Println;
("12") -> Std.String.ParseInt64 -> (_, Half) -> Std.Result.AndThen -> Std.Println;
("7") -> Std.String.ParseInt64 -> (_, Half) -> Std.Result.AndThen -> Std.Println;
("x") -> Std.String.ParseInt64 -> (_, Zero) -> Std.Result.OrElse -> Std.Println;
("x") -> Std.String.ParseInt64 -> (_, 80) -> Std.Result.UnwrapOr -> Std.Println;
(5) -> Std.Result.Ok -> (_, Double) -> Std.Result.Map -> Std.Result.Unwrap -> Std.Println;
)");
  const auto loaded =
      fleaux::bytecode::load_linked_module(directory / "main.fleaux", fleaux::bytecode::ModuleLoadOptions{.write_bytecode_cache = false});
  if (!loaded) {
    INFO("load error: " << loaded.error().message << " " << loaded.error().hint.value_or(""));
  }
  REQUIRE(loaded.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*loaded, output);
  if (!executed) {
    INFO("run error: " << executed.error().message);
  }
  REQUIRE(executed.has_value());
  REQUIRE(output.str() ==
          "(True, 42)\n"
          "True\n"
          "42\n"
          "(True, 6)\n"
          "(False, odd)\n"
          "(True, 0)\n"
          "80\n"
          "10\n");
}

TEST_CASE("The functions E1 added type-check and run in a program", "[runtime][tuple][dict]") {
  const auto directory = std::filesystem::temp_directory_path() / "fleaux_runtime_calls_tests" / "new_functions";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  fleaux::tests::write_text_file(directory / "main.fleaux", R"(import Std;
let Words: Tuple<String...> = ("pear", "fig", "apple", "kiwi", "plum");
let Count(n: Int64): Int64 = (n, 1) -> Std.Add;
let Tally(counts: Dict<Int64, Int64>, w: String): Dict<Int64, Int64> =
    (counts, w -> Std.String.Length, 0, Count) -> Std.Dict.Update;
let NoCounts(): Dict<Int64, Int64> = {
    let empty: Dict<Int64, Int64> = () -> Std.Dict.Create;
    empty;
};

(Words) -> Std.Tuple.Enumerate -> (_, 1) -> Std.ElementAt -> Std.Println;
(Words, Std.String.Length) -> Std.Tuple.SortBy -> Std.Println;
(Words, Std.String.Length) -> Std.Tuple.GroupBy -> (_, 4) -> Std.Dict.Get -> Std.Result.Unwrap -> Std.Println;
(Words, (w: String): Bool = (w, "p") -> Std.String.StartsWith) -> Std.Tuple.Partition -> Std.Println;
((1, 2, 3), (n: Int64): Tuple<Int64...> = (n) -> Std.Tuple.Range) -> Std.Tuple.FlatMap -> Std.Println;
((1, 2, 3, 4), 0, Std.Add) -> Std.Tuple.Scan -> Std.Println;
((1, 3, 4, 5), (n: Int64): Bool = (n, 2) -> Std.Mod -> (_, 1) -> Std.Equal) -> Std.Tuple.TakeWhile -> Std.Println;
(Words, () -> NoCounts, Tally) -> Std.Tuple.Reduce -> Std.Dict.Entries -> Std.Println;
((3, -2) -> Std.Min, (3, -2) -> Std.Max, -4 -> Std.Math.Abs, (15, 0, 10) -> Std.Math.Clamp) -> Std.Println;
("ab", 2) -> Std.String.Repeat -> Std.String.Reverse -> Std.String.Chars -> Std.Println;
)");
  const auto loaded =
      fleaux::bytecode::load_linked_module(directory / "main.fleaux", fleaux::bytecode::ModuleLoadOptions{.write_bytecode_cache = false});
  if (!loaded) {
    INFO("load error: " << loaded.error().message << " " << loaded.error().hint.value_or(""));
  }
  REQUIRE(loaded.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*loaded, output);
  if (!executed) {
    INFO("run error: " << executed.error().message);
  }
  REQUIRE(executed.has_value());
  REQUIRE(output.str() ==
          "(1, fig)\n"
          "(fig, pear, kiwi, plum, apple)\n"
          "(pear, kiwi, plum)\n"
          "((pear, plum), (fig, apple, kiwi))\n"
          "(0, 0, 1, 0, 1, 2)\n"
          "(1, 3, 6, 10)\n"
          "(1, 3)\n"
          "((4, 3), (3, 1), (5, 1))\n"
          "-2 3 4 10\n"
          "(b, a, b, a)\n");
}
