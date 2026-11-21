#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/runtime/runtime_support.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"
#include "vm_test_support.hpp"

namespace {

auto push_i64_const(fleaux::bytecode::Module& bytecode_module, const std::int64_t value) -> std::int64_t {
  bytecode_module.constants.emplace_back(value);
  return static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
}

auto push_f64_const(fleaux::bytecode::Module& bytecode_module, const double value) -> std::int64_t {
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{value});
  return static_cast<std::int64_t>(bytecode_module.constants.size() - 1U);
}

auto push_u64_const(fleaux::bytecode::Module& bytecode_module, const std::uint64_t value) -> std::int64_t {
  bytecode_module.constants.emplace_back(value);
  return static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
}

constexpr auto builtin(const fleaux::vm::BuiltinId builtin_id) -> std::int64_t {
  return fleaux::vm::builtin_operand(builtin_id);
}

constexpr auto builtin_direct(const fleaux::vm::BuiltinId builtin_id) -> std::int64_t {
  return fleaux::vm::builtin_operand(builtin_id, fleaux::vm::BuiltinCallShape::kDirectValue);
}

}  // namespace

TEST_CASE("VM executes arithmetic bytecode and prints result", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c7 = push_i64_const(bytecode_module, 7);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    FAIL(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM invocation options route output through the configured stream", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c7 = push_i64_const(bytecode_module, 7);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result =
      runtime.execute(bytecode_module, fleaux::vm::RuntimeInvocationOptions{.output = std::ref(output)});

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "7\n");
}

TEST_CASE("VM rejects invalid opcode values with a controlled runtime error", "[vm][opcodes]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.instructions = {
      {.opcode = static_cast<fleaux::bytecode::Opcode>(9999), .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("invalid opcode 9999") != std::string::npos);
}

TEST_CASE("VM Std.Printf does not append trailing newline", "[vm][printf]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintfBuiltin = builtin(fleaux::vm::BuiltinId::Printf);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"{} + {} = {}"}});
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintfBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    FAIL(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 + 2 = 3");
}

TEST_CASE("VM kBuildTuple preserves stack order for builtin indexing", "[vm][tuple]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c11 = push_i64_const(bytecode_module, 11);
  const auto c22 = push_i64_const(bytecode_module, 22);
  const auto c33 = push_i64_const(bytecode_module, 33);
  const auto c1 = push_i64_const(bytecode_module, 1);
  constexpr auto kElementAtBuiltin = builtin(fleaux::vm::BuiltinId::ElementAt);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c22},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c33},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kElementAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "22\n");
}

TEST_CASE("VM kBuildTuple preserves the first element when collapsing stack tail in place", "[vm][tuple]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c11 = push_i64_const(bytecode_module, 11);
  const auto c22 = push_i64_const(bytecode_module, 22);
  const auto c33 = push_i64_const(bytecode_module, 33);
  const auto c0 = push_i64_const(bytecode_module, 0);
  constexpr auto kElementAtBuiltin = builtin(fleaux::vm::BuiltinId::ElementAt);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c22},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c33},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kElementAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "11\n");
}

TEST_CASE("RuntimeSession preserves typed lets across snippets", "[vm][repl][type]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto define_result =
      session.run_snippet("import Std;\nlet AddOne(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n", output);
  REQUIRE(define_result.has_value());

  const auto use_result = session.run_snippet("import Std;\n2.0 -> AddOne -> Std.Println;\n", output);
  if (!use_result.has_value()) {
    INFO("vm repl typed let lookup error: " << use_result.error().message);
  }
  REQUIRE(use_result.has_value());
  REQUIRE(output.str() == "3\n");
}

TEST_CASE("RuntimeSession scopes configured process args to snippet execution", "[vm][repl]") {
  fleaux::runtime::set_process_args(std::vector<std::string>{"outer", "persisted"});

  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({"inner", "beta"});
  std::ostringstream output;

  const auto result = session.run_snippet("import Std;\n() -> Std.GetArgs -> Std.Length -> Std.Println;\n", output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n");

  const auto ambient_args = fleaux::runtime::get_process_args();
  REQUIRE(ambient_args.size() == 2U);
  REQUIRE(ambient_args[0] == "outer");
  REQUIRE(ambient_args[1] == "persisted");
}

TEST_CASE("RuntimeSession scopes explicit input to snippet execution", "[vm][repl]") {
  std::istringstream ambient_input{"Outer\n"};
  fleaux::runtime::RuntimeInputStreamScope ambient_scope(ambient_input);

  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;
  std::istringstream provided_input{"Ada\n"};

  const auto result = session.run_snippet("import Std;\n() -> Std.Input -> Std.Println;\n", output, provided_input);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "Ada\n");

  std::string ambient_line;
  REQUIRE(std::getline(ambient_input, ambient_line));
  REQUIRE(ambient_line == "Outer");
}

TEST_CASE("RuntimeSession supports the prompt overload of Std.Input", "[vm][repl]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;
  std::istringstream provided_input{"Ada\n"};

  const auto result =
      session.run_snippet("import Std;\n\"name> \" -> Std.Input -> Std.Println;\n", output, provided_input);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "name> Ada\n");
}

TEST_CASE("RuntimeSession uses scoped Std help metadata without clobbering ambient help registry", "[vm][repl]") {
  fleaux::runtime::clear_help_metadata_registry();
  fleaux::runtime::register_help_metadata(fleaux::runtime::HelpMetadata{
      .name = "Host.Custom",
      .signature = "let Host.Custom(): String",
      .doc_lines = {"@brief Host-only help entry."},
      .is_builtin = false,
  });

  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet("import Std;\n(\"Std.Add\") -> Std.Help -> Std.Println;\n", output);

  REQUIRE(result.has_value());
  REQUIRE(output.str().find("Help on function Std.Add") != std::string::npos);

  const auto ambient_help = fleaux::runtime::Help(fleaux::runtime::make_string("Host.Custom"));
  REQUIRE(fleaux::runtime::as_string(ambient_help).find("Host.Custom") != std::string::npos);

  fleaux::runtime::clear_help_metadata_registry();
}

TEST_CASE("RuntimeSession preserves strong type declarations across snippets", "[vm][repl][type][strong]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto define_result = session.run_snippet("type UserId = Int64;\nlet Echo(x: UserId): UserId = x;\n", output);
  if (!define_result.has_value()) {
    INFO("vm repl strong type definition error: " << define_result.error().message);
  }
  REQUIRE(define_result.has_value());

  const auto use_type_result = session.run_snippet("let Wrap(x: UserId): UserId = x;\n", output);
  if (!use_type_result.has_value()) {
    INFO("vm repl strong type reuse error: " << use_type_result.error().message);
  }
  REQUIRE(use_type_result.has_value());

  const auto mismatch_result = session.run_snippet("(1) -> Wrap;\n", output);
  REQUIRE_FALSE(mismatch_result.has_value());
  REQUIRE(mismatch_result.error().message == "Type mismatch in call target arguments.");
  REQUIRE(mismatch_result.error().hint.has_value());
  REQUIRE_THAT(*mismatch_result.error().hint, Catch::Matchers::ContainsSubstring("Wrap expects argument 0"));
}

TEST_CASE("RuntimeSession preserves transparent aliases across snippets", "[vm][repl][type][aliases]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto define_result =
      session.run_snippet("import Std;\nalias Name = String;\nlet Echo(x: Name): Name = x;\n", output);
  if (!define_result.has_value()) {
    INFO("vm repl alias definition error: " << define_result.error().message);
  }
  REQUIRE(define_result.has_value());

  const auto use_alias_result = session.run_snippet(
      "import Std;\nlet NeedsName(x: Name): Name = x;\n(\"fleaux\") -> Echo -> NeedsName -> Std.Println;\n", output);
  if (!use_alias_result.has_value()) {
    INFO("vm repl alias reuse error: " << use_alias_result.error().message);
  }
  REQUIRE(use_alias_result.has_value());
  REQUIRE(output.str() == "fleaux\n");
}

TEST_CASE("RuntimeSession replaces transparent aliases by name across snippets", "[vm][repl][type][aliases]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto define_string_alias =
      session.run_snippet("import Std;\nalias Value = String;\nlet Echo(value: Value): Value = value;\n", output);
  if (!define_string_alias.has_value()) {
    INFO("vm repl initial alias definition error: " << define_string_alias.error().message);
  }
  REQUIRE(define_string_alias.has_value());

  const auto replace_alias =
      session.run_snippet("import Std;\nalias Value = Int64;\nlet NeedsValue(value: Value): Value = value;\n", output);
  if (!replace_alias.has_value()) {
    INFO("vm repl alias replacement error: " << replace_alias.error().message);
  }
  REQUIRE(replace_alias.has_value());

  const auto use_replaced_alias = session.run_snippet("import Std;\n(1) -> NeedsValue -> Std.Println;\n", output);
  if (!use_replaced_alias.has_value()) {
    INFO("vm repl replaced alias reuse error: " << use_replaced_alias.error().message);
  }
  REQUIRE(use_replaced_alias.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("RuntimeSession requires Std import before Std symbols are available", "[vm][repl][imports][stdlib]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto missing_import_result = session.run_snippet("(1, 2) -> Std.Add -> Std.Println;\n", output);
  REQUIRE_FALSE(missing_import_result.has_value());
  REQUIRE(missing_import_result.error().message == "Unresolved symbol.");
  REQUIRE(missing_import_result.error().hint.has_value());
  REQUIRE(((*missing_import_result.error().hint).find("Std.Add") != std::string::npos ||
           (*missing_import_result.error().hint).find("Std.Println") != std::string::npos));

  const auto import_result = session.run_snippet("import Std;\n", output);
  if (!import_result.has_value()) {
    INFO(import_result.error().message);
  }
  REQUIRE(import_result.has_value());

  const auto use_result = session.run_snippet("(1, 2) -> Std.Add -> Std.Println;\n", output);
  if (!use_result.has_value()) {
    INFO(use_result.error().message);
  }
  REQUIRE(use_result.has_value());
  REQUIRE(output.str() == "3\n");
}

TEST_CASE("RuntimeSession type-checks later snippets against prior lets", "[vm][repl][type]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto define_result =
      session.run_snippet("import Std;\nlet AddOne(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n", output);
  REQUIRE(define_result.has_value());

  const auto mismatch_result = session.run_snippet("\"oops\" -> AddOne;\n", output);
  REQUIRE_FALSE(mismatch_result.has_value());
  REQUIRE(mismatch_result.error().message == "Type mismatch in call target arguments.");
  REQUIRE(mismatch_result.error().hint.has_value());
  REQUIRE_THAT(*mismatch_result.error().hint, Catch::Matchers::ContainsSubstring("AddOne expects argument 0"));
}

TEST_CASE("RuntimeSession executes zero-arg inline closure pipeline sugar", "[vm][repl][closure]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet("import Std;\n() -> (): Any = (\"Empty Closure\") -> Std.Println;\n", output);
  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "Empty Closure\n");
}

TEST_CASE("RuntimeSession preserves overloads across snippets", "[vm][repl][type][overload]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream sink;

  REQUIRE(session.run_snippet("import Std;\nlet FuncA(): Any = (\"FuncA\") -> Std.Println;\n", sink).has_value());
  REQUIRE(session.run_snippet("import Std;\nlet FuncA(x: Int64): Any = (\"FuncA: x: Int64\") -> Std.Println;\n", sink)
              .has_value());
  REQUIRE(session.run_snippet("import Std;\nlet FuncA(x: String): Any = (\"FuncA: x: String\") -> Std.Println;\n", sink)
              .has_value());

  std::ostringstream nullary_output;
  const auto nullary_result = session.run_snippet("import Std;\n() -> FuncA;\n", nullary_output);
  if (!nullary_result.has_value()) {
    INFO(nullary_result.error().message);
  }
  REQUIRE(nullary_result.has_value());
  REQUIRE(nullary_output.str() == "FuncA\n");

  std::ostringstream int_output;
  const auto int_result = session.run_snippet("import Std;\n(1) -> FuncA;\n", int_output);
  if (!int_result.has_value()) {
    INFO(int_result.error().message);
  }
  REQUIRE(int_result.has_value());
  REQUIRE(int_output.str() == "FuncA: x: Int64\n");

  std::ostringstream string_output;
  const auto string_result = session.run_snippet("import Std;\n(\"hello\") -> FuncA;\n", string_output);
  if (!string_result.has_value()) {
    INFO(string_result.error().message);
  }
  REQUIRE(string_result.has_value());
  REQUIRE(string_output.str() == "FuncA: x: String\n");
}

TEST_CASE("RuntimeSession exact overload redefinition replaces only the matching overload",
          "[vm][repl][type][overload]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream sink;

  REQUIRE(session.run_snippet("import Std;\nlet FuncA(): Any = (\"FuncA\") -> Std.Println;\n", sink).has_value());
  REQUIRE(session.run_snippet("import Std;\nlet FuncA(x: Int64): Any = (\"FuncA: x: Int64\") -> Std.Println;\n", sink)
              .has_value());
  REQUIRE(session.run_snippet("import Std;\nlet FuncA(x: String): Any = (\"FuncA: x: String\") -> Std.Println;\n", sink)
              .has_value());

  REQUIRE(
      session
          .run_snippet("import Std;\nlet FuncA(x: Int64): Any = (\"FuncA: x: Int64 updated\") -> Std.Println;\n", sink)
          .has_value());

  std::ostringstream nullary_output;
  const auto nullary_result = session.run_snippet("import Std;\n() -> FuncA;\n", nullary_output);
  REQUIRE(nullary_result.has_value());
  REQUIRE(nullary_output.str() == "FuncA\n");

  std::ostringstream int_output;
  const auto int_result = session.run_snippet("import Std;\n(1) -> FuncA;\n", int_output);
  REQUIRE(int_result.has_value());
  REQUIRE(int_output.str() == "FuncA: x: Int64 updated\n");

  std::ostringstream string_output;
  const auto string_result = session.run_snippet("import Std;\n(\"hello\") -> FuncA;\n", string_output);
  REQUIRE(string_result.has_value());
  REQUIRE(string_output.str() == "FuncA: x: String\n");
}

TEST_CASE("RuntimeSession executes top-level immutable value declarations", "[vm][repl][blocks][let]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet("import Std;\nlet Meaning: Int64 = 42;\nMeaning -> Std.Println;\n", output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("RuntimeSession executes block-scoped locals and nested shadowing", "[vm][repl][blocks]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Compute(): Int64 = {\n"
      "  let x: Int64 = 1;\n"
      "  let y: Int64 = {\n"
      "    let x: Int64 = 2;\n"
      "    (x, 1) -> Std.Add;\n"
      "  };\n"
      "  (x, y) -> Std.Add;\n"
      "};\n"
      "() -> Compute -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "4\n");
}

TEST_CASE("RuntimeSession type-checks Std imports using canonical stdlib declarations", "[vm][repl][type][stdlib]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto mismatch_result = session.run_snippet("import Std;\n((1, 2, 3), 1.5) -> Std.Array.GetAt;\n", output);
  REQUIRE_FALSE(mismatch_result.has_value());
  REQUIRE(mismatch_result.error().message == "Type mismatch in call target arguments.");
  REQUIRE(mismatch_result.error().hint.has_value());
  REQUIRE_THAT(*mismatch_result.error().hint,
               Catch::Matchers::ContainsSubstring("Std.Array.GetAt expects argument 1 to match declared type"));
}

TEST_CASE("RuntimeSession resolves Std.String parse builtins from canonical stdlib declarations",
          "[vm][repl][type][stdlib]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "(\"-42\") -> Std.String.ParseInt64 -> Std.Println;\n"
      "(\"42\") -> Std.String.ParseUInt64 -> Std.Println;\n"
      "(\"6.25e1\") -> Std.String.ParseFloat64 -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "-42\n42\n62.5\n");
}

TEST_CASE("RuntimeSession preserves parenthesized singleton lhs for unary string builtins", "[vm][repl][type][stdlib]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "(\"  Hello  \") -> Std.String.Trim -> Std.Println;\n"
      "(\"abcd\") -> Std.String.Length -> Std.Println;\n"
      "(\"6.5\") -> Std.ToNum -> Std.Println;\n"
      "(\"41\") -> Std.String.ParseInt64 -> (x: Int64): Int64 = (x, 1) -> Std.Add -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "Hello\n4\n6.5\n42\n");
}

TEST_CASE("RuntimeSession preserves parenthesized singleton lhs for unary path builtins", "[vm][repl][type][stdlib]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "(\"a/b/c.txt\") -> Std.Path.Basename -> Std.Println;\n"
      "(\"a/b/c.txt\") -> Std.Path.Extension -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "c.txt\n.txt\n");
}

TEST_CASE("RuntimeSession preserves parenthesized singleton lhs for unary user functions", "[vm][repl][type]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Inspect(x: Tuple<Int64>): Int64 = x -> Std.Length;\n"
      "((1)) -> Inspect -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("RuntimeSession Std.Apply preserves parenthesized singleton lhs for user callables", "[vm][repl][type]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Inspect(x: Tuple<Int64>): Int64 = x -> Std.Length;\n"
      "((1,), Inspect) -> Std.Apply -> Std.Println;\n"
      "((1,), (x: Tuple<Int64>): Int64 = x -> Std.Length) -> Std.Apply -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n1\n");
}

TEST_CASE("RuntimeSession Std.Help shows canonical stdlib docs", "[vm][repl][help][stdlib]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_repl_help_embedded_std_only";
  std::filesystem::remove_all(temp_dir);
  const auto poisoned_fallbacks = fleaux::tests::write_poisoned_symbolic_std_fallbacks(temp_dir);

  const fleaux::tests::CurrentPathScope current_path_scope(temp_dir);
  const fleaux::tests::ScopedEnvVar env_scope("FLEAUX_STD_PATH", poisoned_fallbacks.env_std_path.string());

  fleaux::runtime::clear_help_metadata_registry();
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto help_result = session.run_snippet("import Std;\n(\"Std.Add\") -> Std.Help -> Std.Println;\n", output);
  if (!help_result.has_value()) {
    INFO("repl help error: " << help_result.error().message);
  }
  REQUIRE(help_result.has_value());
  REQUIRE_THAT(output.str(), Catch::Matchers::ContainsSubstring("Help on function Std.Add"));
  REQUIRE_THAT(output.str(), Catch::Matchers::ContainsSubstring("Add two numbers of the same numeric type."));
  REQUIRE_THAT(output.str(), Catch::Matchers::ContainsSubstring("Parameters:"));
}

TEST_CASE("VM executes value-ref opcodes round-trip", "[vm][lifetime][value_ref]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_i64_const(bytecode_module, 10);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeValueRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kDerefValueRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM execute reclaims transient callable refs", "[vm][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::bytecode::Module bytecode_module;
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = builtin(fleaux::vm::BuiltinId::UnaryPlus)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = builtin(fleaux::vm::BuiltinId::UnaryPlus)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;

  const auto first = runtime.execute(bytecode_module, output);
  REQUIRE(first.has_value());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto second = runtime.execute(bytecode_module, output);
  REQUIRE(second.has_value());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
}

TEST_CASE("RuntimeSession run_snippet reclaims transient callable refs across runs", "[vm][repl][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});

  const std::string snippet =
      "import Std;\n"
      "let MakeAdder(n: Float64): (Float64) => Float64 = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
      "(10.0, (4.0) -> MakeAdder) -> Std.Apply -> Std.Println;\n";

  for (int iter = 0; iter < 25; ++iter) {
    std::ostringstream output;
    const auto result = session.run_snippet(snippet, output);
    REQUIRE(result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }
}

TEST_CASE("VM execute invalidates escaped transient callable refs", "[vm][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::bytecode::Module bytecode_module;
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = builtin(fleaux::vm::BuiltinId::UnaryPlus)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);
  REQUIRE(result.has_value());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::Array escaped_ref;
  escaped_ref.Reserve(2);
  escaped_ref.PushBack(fleaux::runtime::Value{fleaux::runtime::String{fleaux::runtime::k_callable_tag}});
  escaped_ref.PushBack(fleaux::runtime::Value{fleaux::runtime::UInt{0}});

  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(fleaux::runtime::Value{std::move(escaped_ref)},
                                                           fleaux::runtime::make_int(7)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("Callable refs reject stale generations after slot reuse", "[vm][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::Value stale_ref;
  fleaux::runtime::CallableId stale_id{.slot = 0, .generation = 0};
  {
    fleaux::runtime::CallableRegistryScope scope;
    stale_ref =
        fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });

    const auto parsed = fleaux::runtime::callable_id_from_value(stale_ref);
    REQUIRE(parsed.has_value());
    stale_id = *parsed;
    const auto stale_result = fleaux::runtime::invoke_callable_ref(stale_ref, fleaux::runtime::make_int(9));
    REQUIRE(fleaux::runtime::as_int_value(stale_result) == 9);
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::Value live_ref;
  fleaux::runtime::CallableId live_id{.slot = 0, .generation = 0};
  {
    fleaux::runtime::CallableRegistryScope scope;
    live_ref =
        fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });

    const auto parsed = fleaux::runtime::callable_id_from_value(live_ref);
    REQUIRE(parsed.has_value());
    live_id = *parsed;
    REQUIRE(stale_id.slot == live_id.slot);
    REQUIRE(stale_id.generation != live_id.generation);

    REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(stale_ref, fleaux::runtime::make_int(1)),
                        Catch::Matchers::ContainsSubstring("Unknown callable reference"));
    const auto live_result = fleaux::runtime::invoke_callable_ref(live_ref, fleaux::runtime::make_int(11));
    REQUIRE(fleaux::runtime::as_int_value(live_result) == 11);
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
}

TEST_CASE("PinnedCallableRef survives CallableRegistryScope teardown", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  auto pinned = fleaux::runtime::make_pinned_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
    return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) * 3);
  });

  REQUIRE(pinned.is_valid());
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

  {
    // Transient callables created inside this scope must be cleaned up.
    fleaux::runtime::CallableRegistryScope scope;
    fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
    fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
    REQUIRE(fleaux::runtime::callable_registry_size() == 3U);
  }

  // Transients gone; pinned ref must still be alive.
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
  REQUIRE(pinned.is_valid());

  const auto result = fleaux::runtime::invoke_callable_ref(pinned.token(), fleaux::runtime::make_int(7));
  REQUIRE(fleaux::runtime::as_int_value(result) == 21);
}

TEST_CASE("PinnedCallableRef explicit release invalidates the token", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  auto pinned = fleaux::runtime::make_pinned_callable_ref(
      [](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });

  REQUIRE(pinned.is_valid());
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

  const auto token_copy = pinned.token();
  pinned.release();

  REQUIRE_FALSE(pinned.is_valid());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(token_copy, fleaux::runtime::make_int(1)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("PinnedCallableRef RAII releases on destruction", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::Value token_copy;
  {
    auto pinned = fleaux::runtime::make_pinned_callable_ref(
        [](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
    REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
    token_copy = pinned.token();
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(token_copy, fleaux::runtime::make_int(1)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("PinnedCallableRef slot is reused by transient after release", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::CallableId pinned_id{};
  fleaux::runtime::Value stale_token;
  {
    auto pinned = fleaux::runtime::make_pinned_callable_ref(
        [](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
    pinned_id = pinned.id();
    stale_token = pinned.token();
    // pinned destroyed here
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  // A new transient callable must reuse the freed slot.
  fleaux::runtime::Value live_token;
  fleaux::runtime::CallableId live_id{};
  {
    fleaux::runtime::CallableRegistryScope scope;
    live_token = fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
      return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 100);
    });
    const auto parsed = fleaux::runtime::callable_id_from_value(live_token);
    REQUIRE(parsed.has_value());
    live_id = *parsed;

    // Same slot, different generation.
    REQUIRE(live_id.slot == pinned_id.slot);
    REQUIRE(live_id.generation != pinned_id.generation);

    // Stale token must be rejected.
    REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(stale_token, fleaux::runtime::make_int(1)),
                        Catch::Matchers::ContainsSubstring("Unknown callable reference"));

    // Live token works fine.
    const auto result = fleaux::runtime::invoke_callable_ref(live_token, fleaux::runtime::make_int(5));
    REQUIRE(fleaux::runtime::as_int_value(result) == 105);
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
}

TEST_CASE("release_callable_ref Value overload retires pinned slot", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto id = fleaux::runtime::register_callable_pinned(
      [](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

  fleaux::runtime::Array token_arr;
  token_arr.Reserve(3);
  token_arr.PushBack(fleaux::runtime::Value{fleaux::runtime::String{fleaux::runtime::k_callable_tag}});
  token_arr.PushBack(fleaux::runtime::Value{id.slot});
  token_arr.PushBack(fleaux::runtime::Value{id.generation});
  const fleaux::runtime::Value token{std::move(token_arr)};

  REQUIRE_NOTHROW(fleaux::runtime::invoke_callable_ref(token, fleaux::runtime::make_null()));

  fleaux::runtime::release_callable_ref(token);
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(token, fleaux::runtime::make_null()),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("Nested CallableRegistryScope keeps outer transient callable alive until outer teardown", "[vm][lifetime]") {
  fleaux::runtime::reset_callable_registry();

  fleaux::runtime::Value outer_token;
  fleaux::runtime::Value inner_token;
  {
    fleaux::runtime::CallableRegistryScope outer_scope;
    outer_token = fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
      return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 1);
    });
    REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

    {
      fleaux::runtime::CallableRegistryScope inner_scope;
      inner_token = fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 10);
      });
      REQUIRE(fleaux::runtime::callable_registry_size() == 2U);
      REQUIRE(fleaux::runtime::as_int_value(
                  fleaux::runtime::invoke_callable_ref(outer_token, fleaux::runtime::make_int(4))) == 5);
      REQUIRE(fleaux::runtime::as_int_value(
                  fleaux::runtime::invoke_callable_ref(inner_token, fleaux::runtime::make_int(4))) == 14);
    }

    REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
    REQUIRE(fleaux::runtime::as_int_value(
                fleaux::runtime::invoke_callable_ref(outer_token, fleaux::runtime::make_int(9))) == 10);
    REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(inner_token, fleaux::runtime::make_int(1)),
                        Catch::Matchers::ContainsSubstring("Unknown callable reference"));
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(outer_token, fleaux::runtime::make_int(1)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("PinnedCallableRef move assignment preserves token validity and single ownership", "[vm][lifetime][pinned]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::runtime::PinnedCallableRef moved;
  fleaux::runtime::Value token_copy;
  {
    auto pinned = fleaux::runtime::make_pinned_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
      return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) * 4);
    });
    token_copy = pinned.token();
    moved = std::move(pinned);

    REQUIRE_FALSE(pinned.is_valid());
    REQUIRE(moved.is_valid());
    REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
  }

  REQUIRE(fleaux::runtime::as_int_value(
              fleaux::runtime::invoke_callable_ref(moved.token(), fleaux::runtime::make_int(6))) == 24);
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

  moved.release();
  REQUIRE_FALSE(moved.is_valid());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(token_copy, fleaux::runtime::make_int(1)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
}

TEST_CASE("Tagged token helpers preserve callable, value-ref, and handle ids", "[vm][lifetime][tokens]") {
  fleaux::runtime::reset_callable_registry();
  {
    fleaux::runtime::CallableRegistryScope scope;
    const auto callable_token =
        fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value { return arg; });
    const auto callable_id = fleaux::runtime::callable_id_from_value(callable_token);
    REQUIRE(callable_id.has_value());

    fleaux::runtime::Array legacy_callable_token;
    legacy_callable_token.Reserve(2);
    legacy_callable_token.EmplaceBack(fleaux::runtime::String{fleaux::runtime::k_callable_tag});
    legacy_callable_token.EmplaceBack(callable_id->slot);

    const auto legacy_callable_id =
        fleaux::runtime::callable_id_from_value(fleaux::runtime::Value{std::move(legacy_callable_token)});
    REQUIRE(legacy_callable_id.has_value());
    REQUIRE(legacy_callable_id->slot == callable_id->slot);
    REQUIRE(legacy_callable_id->generation == 0U);
  }

  {
    fleaux::runtime::ValueRegistryScope scope;
    const auto value_ref_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(123));
    const auto value_ref_id = fleaux::runtime::value_ref_id_from_token(value_ref_token);
    REQUIRE(value_ref_id.has_value());
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(value_ref_token)) == 123);
  }

  const auto handle_id = fleaux::runtime::handle_id_from_value(fleaux::runtime::make_handle_token(5U, 7U));
  REQUIRE(handle_id.has_value());
  REQUIRE(handle_id->slot == 5U);
  REQUIRE(handle_id->gen == 7U);
}

// ============================================================================
// ValueRegistry tests
// ============================================================================

TEST_CASE("ValueRegistryScope cleans up transient value refs", "[vm][lifetime][value_ref]") {
  fleaux::runtime::Value stale_token;
  {
    fleaux::runtime::ValueRegistryScope scope;
    stale_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(42));
    const auto val = fleaux::runtime::deref_value_ref(stale_token);
    REQUIRE(fleaux::runtime::as_int_value(val) == 42);
  }
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(stale_token),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));
}

TEST_CASE("PinnedValueRef survives ValueRegistryScope teardown", "[vm][lifetime][value_ref]") {
  auto pinned = fleaux::runtime::PinnedValueRef{fleaux::runtime::make_int(99)};
  REQUIRE(pinned.is_valid());
  {
    fleaux::runtime::ValueRegistryScope scope;
    const auto transient_a = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(1));
    const auto transient_b = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(2));
    REQUIRE(transient_a.HasArray());
    REQUIRE(transient_b.HasArray());
  }
  // Pinned value must still be reachable.
  REQUIRE(pinned.is_valid());
  REQUIRE(fleaux::runtime::as_int_value(pinned.get()) == 99);
}

TEST_CASE("PinnedValueRef explicit release invalidates token", "[vm][lifetime][value_ref]") {
  auto pinned = fleaux::runtime::PinnedValueRef{fleaux::runtime::make_string("hello")};
  const auto token_copy = pinned.token();
  pinned.release();
  REQUIRE_FALSE(pinned.is_valid());
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(token_copy),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));
}

TEST_CASE("PinnedValueRef RAII releases on destruction", "[vm][lifetime][value_ref]") {
  fleaux::runtime::Value token_copy;
  {
    auto pinned = fleaux::runtime::PinnedValueRef{fleaux::runtime::make_bool(true)};
    token_copy = pinned.token();
    REQUIRE(fleaux::runtime::deref_value_ref(token_copy).TryGetBool().has_value());
  }
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(token_copy),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));
}

TEST_CASE("Nested ValueRegistryScope keeps outer transient value alive until outer teardown",
          "[vm][lifetime][value_ref]") {
  fleaux::runtime::reset_value_registry_for_tests();

  fleaux::runtime::Value outer_token;
  fleaux::runtime::Value inner_token;
  {
    fleaux::runtime::ValueRegistryScope outer_scope;
    outer_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(12));
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(outer_token)) == 12);

    {
      fleaux::runtime::ValueRegistryScope inner_scope;
      inner_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(34));
      REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(inner_token)) == 34);
      REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(outer_token)) == 12);
    }

    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(outer_token)) == 12);
    REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(inner_token),
                        Catch::Matchers::ContainsSubstring("stale or unknown"));
  }

  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(outer_token),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));
  fleaux::runtime::reset_value_registry_for_tests();
}

TEST_CASE("PinnedValueRef move assignment preserves token validity and single ownership", "[vm][lifetime][value_ref]") {
  fleaux::runtime::reset_value_registry_for_tests();

  fleaux::runtime::PinnedValueRef moved;
  fleaux::runtime::Value token_copy;
  {
    auto pinned = fleaux::runtime::PinnedValueRef{fleaux::runtime::make_string("kept")};
    token_copy = pinned.token();
    moved = std::move(pinned);

    REQUIRE_FALSE(pinned.is_valid());
    REQUIRE(moved.is_valid());
  }

  REQUIRE(fleaux::runtime::as_string(moved.get()) == "kept");
  moved.release();
  REQUIRE_FALSE(moved.is_valid());
  REQUIRE_THROWS_WITH(moved.get(), Catch::Matchers::ContainsSubstring("already released"));
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(token_copy),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));
  fleaux::runtime::reset_value_registry_for_tests();
}

TEST_CASE("ActiveRuntimeExecutionStateScope isolates callable and value registries from the default state",
          "[vm][lifetime][execution_state]") {
  fleaux::runtime::reset_callable_registry();
  fleaux::runtime::reset_value_registry_for_tests();

  auto default_callable =
      fleaux::runtime::make_pinned_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) + 100);
      });
  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);

  fleaux::runtime::Value isolated_callable;
  fleaux::runtime::Value isolated_value_ref;
  fleaux::runtime::RuntimeExecutionState isolated_state;
  {
    fleaux::runtime::ActiveRuntimeExecutionStateScope state_scope(isolated_state);
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
    REQUIRE(fleaux::runtime::value_registry_telemetry().active_count == 0U);

    fleaux::runtime::CallableRegistryScope callable_scope;
    fleaux::runtime::ValueRegistryScope value_scope;

    isolated_callable = fleaux::runtime::make_callable_ref([](fleaux::runtime::Value arg) -> fleaux::runtime::Value {
      return fleaux::runtime::make_int(fleaux::runtime::as_int_value(arg) * 5);
    });
    isolated_value_ref = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(77));

    REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
    REQUIRE(fleaux::runtime::as_int_value(
                fleaux::runtime::invoke_callable_ref(isolated_callable, fleaux::runtime::make_int(4))) == 20);
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(isolated_value_ref)) == 77);
  }

  REQUIRE(fleaux::runtime::callable_registry_size() == 1U);
  REQUIRE(fleaux::runtime::as_int_value(
              fleaux::runtime::invoke_callable_ref(default_callable.token(), fleaux::runtime::make_int(1))) == 101);
  REQUIRE_THROWS_WITH(fleaux::runtime::invoke_callable_ref(isolated_callable, fleaux::runtime::make_int(1)),
                      Catch::Matchers::ContainsSubstring("Unknown callable reference"));
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(isolated_value_ref),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));

  default_callable.release();
  fleaux::runtime::reset_callable_registry();
  fleaux::runtime::reset_value_registry_for_tests();
}

TEST_CASE("ValueRegistry slot reuse produces different generation", "[vm][lifetime][value_ref]") {
  fleaux::runtime::Value stale_token;
  fleaux::runtime::RegistryId first_id{};
  {
    fleaux::runtime::ValueRegistryScope scope;
    stale_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(7));
    const auto parsed = fleaux::runtime::value_ref_id_from_token(stale_token);
    REQUIRE(parsed.has_value());
    first_id = *parsed;
  }

  fleaux::runtime::Value second_token;
  {
    fleaux::runtime::ValueRegistryScope scope;
    second_token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(8));
    const auto parsed = fleaux::runtime::value_ref_id_from_token(second_token);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->slot == first_id.slot);
    REQUIRE(parsed->generation != first_id.generation);

    REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(stale_token),
                        Catch::Matchers::ContainsSubstring("stale or unknown"));
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(second_token)) == 8);
  }
}

TEST_CASE("release_value_ref explicitly retires a value ref token", "[vm][lifetime][value_ref]") {
  fleaux::runtime::ValueRegistryScope scope;
  const auto token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(55));
  REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(token)) == 55);
  fleaux::runtime::release_value_ref(token);
  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(token), Catch::Matchers::ContainsSubstring("stale or unknown"));
}

TEST_CASE("ValueRegistry telemetry tracks peak active count and stale deref rejections", "[vm][lifetime][value_ref]") {
  fleaux::runtime::reset_value_registry_for_tests();

  const auto initial = fleaux::runtime::value_registry_telemetry();
  REQUIRE(initial.active_count == 0U);
  REQUIRE(initial.peak_active_count == 0U);
  REQUIRE(initial.rejected_allocations == 0U);
  REQUIRE(initial.stale_deref_rejections == 0U);
  REQUIRE_FALSE(initial.transient_cap.has_value());

  fleaux::runtime::Value stale_token;
  {
    fleaux::runtime::ValueRegistryScope scope;
    const auto token_a = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(1));
    const auto token_b = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(2));
    stale_token = token_a;
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(token_b)) == 2);

    const auto during = fleaux::runtime::value_registry_telemetry();
    REQUIRE(during.active_count >= 2U);
    REQUIRE(during.peak_active_count >= 2U);
  }

  REQUIRE_THROWS_WITH(fleaux::runtime::deref_value_ref(stale_token),
                      Catch::Matchers::ContainsSubstring("stale or unknown"));

  const auto after = fleaux::runtime::value_registry_telemetry();
  REQUIRE(after.active_count == 0U);
  REQUIRE(after.peak_active_count >= 2U);
  REQUIRE(after.stale_deref_rejections >= 1U);

  fleaux::runtime::reset_value_registry_for_tests();
}

TEST_CASE("ValueRegistry transient cap rejects new refs and tracks rejections", "[vm][lifetime][value_ref]") {
  fleaux::runtime::reset_value_registry_for_tests();
  fleaux::runtime::set_value_registry_transient_cap(1U);

  {
    fleaux::runtime::ValueRegistryScope scope;
    const auto token = fleaux::runtime::make_value_ref(fleaux::runtime::make_int(7));
    REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::deref_value_ref(token)) == 7);

    REQUIRE_THROWS_WITH(fleaux::runtime::make_value_ref(fleaux::runtime::make_int(8)),
                        Catch::Matchers::ContainsSubstring("transient cap reached"));
  }

  const auto telemetry = fleaux::runtime::value_registry_telemetry();
  REQUIRE(telemetry.rejected_allocations >= 1U);
  REQUIRE(telemetry.transient_cap.has_value());
  REQUIRE(*telemetry.transient_cap == 1U);

  fleaux::runtime::set_value_registry_transient_cap(std::nullopt);
  fleaux::runtime::reset_value_registry_for_tests();
}

// ============================================================================
// HandleRegistryScope tests
// ============================================================================

TEST_CASE("HandleRegistryScope closes abandoned file handles on scope exit", "[vm][lifetime][handle]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / ("fleaux_handle_scope_" + std::to_string(::getpid()) + ".txt");
  std::filesystem::remove(tmp);

  fleaux::runtime::HandleId leaked_id{.slot = 0, .gen = 0};
  {
    fleaux::runtime::HandleRegistryScope scope;
    auto& reg = fleaux::runtime::handle_registry();
    const auto slot = reg.open(tmp.string(), "w");
    leaked_id = fleaux::runtime::HandleId{.slot = slot, .gen = reg.entries[static_cast<std::size_t>(slot)].generation};
    // Do NOT close; scope destructor must handle it.
  }

  // After scope exit the handle must be closed.
  auto* entry = fleaux::runtime::handle_registry().get(leaked_id.slot, leaked_id.gen);
  REQUIRE(entry == nullptr);

  std::filesystem::remove(tmp);
}

TEST_CASE("HandleRegistryScope does not disturb handles opened before checkpoint", "[vm][lifetime][handle]") {
  const auto tmp =
      std::filesystem::temp_directory_path() / ("fleaux_handle_scope_pre_" + std::to_string(::getpid()) + ".txt");
  std::filesystem::remove(tmp);

  auto& reg = fleaux::runtime::handle_registry();
  const auto pre_slot = reg.open(tmp.string(), "w");
  const auto pre_gen = reg.entries[static_cast<std::size_t>(pre_slot)].generation;

  {
    fleaux::runtime::HandleRegistryScope scope;
    // Nothing opened inside scope.
  }

  // Pre-existing handle must still be alive.
  REQUIRE(fleaux::runtime::handle_registry().get(pre_slot, pre_gen) != nullptr);
  fleaux::runtime::handle_registry().close(pre_slot, pre_gen);
  std::filesystem::remove(tmp);
}

TEST_CASE("Nested HandleRegistryScope keeps outer handle alive until outer teardown", "[vm][lifetime][handle]") {
  const auto outer_tmp =
      std::filesystem::temp_directory_path() / ("fleaux_handle_scope_outer_" + std::to_string(::getpid()) + ".txt");
  const auto inner_tmp =
      std::filesystem::temp_directory_path() / ("fleaux_handle_scope_inner_" + std::to_string(::getpid()) + ".txt");
  std::filesystem::remove(outer_tmp);
  std::filesystem::remove(inner_tmp);

  auto& reg = fleaux::runtime::handle_registry();
  fleaux::runtime::HandleId outer_id{.slot = 0, .gen = 0};
  fleaux::runtime::HandleId inner_id{.slot = 0, .gen = 0};
  {
    fleaux::runtime::HandleRegistryScope outer_scope;
    const auto outer_slot = reg.open(outer_tmp.string(), "w");
    outer_id = fleaux::runtime::HandleId{.slot = outer_slot,
                                         .gen = reg.entries[static_cast<std::size_t>(outer_slot)].generation};
    REQUIRE(reg.get(outer_id.slot, outer_id.gen) != nullptr);

    {
      fleaux::runtime::HandleRegistryScope inner_scope;
      const auto inner_slot = reg.open(inner_tmp.string(), "w");
      inner_id = fleaux::runtime::HandleId{.slot = inner_slot,
                                           .gen = reg.entries[static_cast<std::size_t>(inner_slot)].generation};
      REQUIRE(reg.get(inner_id.slot, inner_id.gen) != nullptr);
    }

    REQUIRE(reg.get(outer_id.slot, outer_id.gen) != nullptr);
    REQUIRE(reg.get(inner_id.slot, inner_id.gen) == nullptr);
  }

  REQUIRE(reg.get(outer_id.slot, outer_id.gen) == nullptr);

  std::filesystem::remove(outer_tmp);
  std::filesystem::remove(inner_tmp);
}

TEST_CASE("ActiveRuntimeExecutionStateScope isolates handle registry from the default state",
          "[vm][lifetime][handle][execution_state]") {
  fleaux::runtime::reset_handle_registry_for_tests();

  const auto temp_dir =
      std::filesystem::temp_directory_path() / ("fleaux_handle_state_" + fleaux::runtime::random_suffix());
  std::filesystem::create_directories(temp_dir);
  const auto outer_file = temp_dir / "outer.txt";
  const auto inner_file = temp_dir / "inner.txt";

  {
    std::ofstream out(outer_file);
    out << "outer\n";
  }
  {
    std::ofstream out(inner_file);
    out << "inner\n";
  }

  const auto default_token = fleaux::runtime::FileOpen(fleaux::runtime::make_tuple(
      fleaux::runtime::make_string(outer_file.string()), fleaux::runtime::make_string("r")));
  fleaux::runtime::Value isolated_token = fleaux::runtime::make_null();

  {
    fleaux::runtime::RuntimeExecutionState isolated_state;
    fleaux::runtime::ActiveRuntimeExecutionStateScope state_scope(isolated_state);
    fleaux::runtime::HandleRegistryScope handle_scope;

    isolated_token = fleaux::runtime::FileOpen(fleaux::runtime::make_tuple(
        fleaux::runtime::make_string(inner_file.string()), fleaux::runtime::make_string("r")));

    const auto isolated_read = fleaux::runtime::FileReadLine(isolated_token);
    REQUIRE(fleaux::runtime::as_string(fleaux::runtime::array_at(isolated_read, 1)) == "inner");

    REQUIRE_THROWS_WITH(fleaux::runtime::FileReadLine(default_token),
                        Catch::Matchers::ContainsSubstring("closed or invalid"));
    REQUIRE_FALSE(fleaux::runtime::as_bool(fleaux::runtime::FileClose(default_token)));
  }

  const auto default_read = fleaux::runtime::FileReadLine(default_token);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::array_at(default_read, 1)) == "outer");

  REQUIRE_THROWS_WITH(fleaux::runtime::FileReadLine(isolated_token),
                      Catch::Matchers::ContainsSubstring("closed or invalid"));
  REQUIRE_FALSE(fleaux::runtime::as_bool(fleaux::runtime::FileClose(isolated_token)));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::FileClose(default_token)));

  fleaux::runtime::reset_handle_registry_for_tests();
  std::error_code ec;
  std::filesystem::remove(outer_file, ec);
  std::filesystem::remove(inner_file, ec);
  std::filesystem::remove_all(temp_dir, ec);
}

// ---------------------------------------------------------------------------
// kJump: unconditional jump skips instructions.
//
//  [0] kPushConst idx(10)
//  [1] kJump 3          -> jump to [3], skip [2]
//  [2] kPushConst idx(99) (never reached)
//  [3] kPrint
//  [4] kHalt
// Expected output: "10\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJump skips instructions unconditionally", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c99 = push_i64_const(bytecode_module, 99);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kJump, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c99},  // skipped
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

// ---------------------------------------------------------------------------
// kJumpIf: jump is taken when TOS is true.
//
//  constants[0] = true
//  [0] kPushConst 0     -> push true
//  [1] kJumpIf 3        -> is true -> jump to [3]
//  [2] kNoOp            (skipped)
//  [3] kPushConst 1
//  [4] kPrint
//  [5] kHalt
// Expected output: "77\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf jumps when condition is true", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c77 = push_i64_const(bytecode_module, 77);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},  // push true
      {.opcode = fleaux::bytecode::Opcode::kJumpIf, .operand = 3},     // true -> jump to [3]
      {.opcode = fleaux::bytecode::Opcode::kNoOp, .operand = 0},       // skipped
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c77},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "77\n");
}

// ---------------------------------------------------------------------------
// kJumpIf: jump is NOT taken when TOS is false.
//
//  constants[0] = false
//  [0] kPushConst 1
//  [1] kPushConst 0      -> push false
//  [2] kJumpIf 5         -> is false -> no jump, continue to [3]
//  [3] kPrint            -> prints 55
//  [4] kHalt
// Expected output: "55\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf falls through when condition is false", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c55 = push_i64_const(bytecode_module, 55);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c55},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},  // push false
      {.opcode = fleaux::bytecode::Opcode::kJumpIf, .operand = 5},     // false -> no jump
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},  // prints 55
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "55\n");
}

TEST_CASE("VM kJumpIfNot jumps when condition is false", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c99 = push_i64_const(bytecode_module, 99);
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kJumpIfNot, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c99},  // skipped
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM reports out-of-range jump target", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kJump, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump target out of range");
}

TEST_CASE("VM reports out-of-range kJumpIf target", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kJumpIf, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump_if target out of range");
}

TEST_CASE("VM reports out-of-range kJumpIfNot target", "[vm][jump]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kJumpIfNot, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump_if_not target out of range");
}

TEST_CASE("VM executes native arithmetic and logical opcodes", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kPow, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kCmpLt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kNot, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kAnd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "13\n3\n256\nTrue\nFalse\nTrue\n");
}

TEST_CASE("VM executes native kSelect opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kSelect, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM executes native kBranchCall opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c10 = push_i64_const(bytecode_module, 10);

  fleaux::bytecode::FunctionDef add1;
  add1.name = "AddOne";
  add1.arity = 1;
  add1.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef sub1;
  sub1.name = "SubOne";
  sub1.arity = 1;
  sub1.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(add1));
  bytecode_module.functions.push_back(std::move(sub1));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBranchCall, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\n");
}

TEST_CASE("VM executes kMakeBuiltinFuncRef with native kBranchCall", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(bytecode_module, 10);
  constexpr auto kUnaryMinusBuiltin = builtin(fleaux::vm::BuiltinId::UnaryMinus);
  constexpr auto kUnaryPlusBuiltin = builtin(fleaux::vm::BuiltinId::UnaryPlus);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kUnaryMinusBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kUnaryPlusBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBranchCall, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "-10\n");
}

TEST_CASE("VM executes exact-1 builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  constexpr auto kFirstBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::First);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kFirstBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("VM executes exact-1 Std.Type builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kTypeBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::Type);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kTypeBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "Tuple\n");
}

TEST_CASE("VM executes exact-1 Std.Cast builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c11 = push_i64_const(bytecode_module, 11);
  constexpr auto kCastBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::Cast);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kCastBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9 11\n");
}

TEST_CASE("VM executes exact-1 Std.ToString builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c11 = push_i64_const(bytecode_module, 11);
  constexpr auto kToStringBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ToString);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kToStringBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "(9, 11)\n");
}

TEST_CASE("VM executes exact-1 Std.Tuple.Sort builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kTupleSortBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::TupleSort);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kTupleSortBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n");
}

TEST_CASE("VM executes exact-1 Std.Result builtin refs with direct-value result tuples", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c5 = push_i64_const(bytecode_module, 5);
  constexpr auto kResultOkBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultOk);
  constexpr auto kResultTagBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ResultTag);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultTagBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n");
}

TEST_CASE("VM executes exact-1 Std.Result.Payload builtin refs with direct-value result tuples", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"boom"}});
  const auto cBoom = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  constexpr auto kResultErrBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultErr);
  constexpr auto kResultPayloadBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ResultPayload);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBoom},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultPayloadBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "boom\n");
}

TEST_CASE("VM executes exact-1 Std.Random.NextUInt64 builtin refs with direct-value generator handles", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto cSeed = push_u64_const(bytecode_module, 123456789ULL);
  constexpr auto kRandomCreateBuiltin = builtin_direct(fleaux::vm::BuiltinId::RandomCreate);
  constexpr auto kRandomNextUInt64BuiltinRef = builtin_direct(fleaux::vm::BuiltinId::RandomNextUInt64);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kFirstBuiltin = builtin_direct(fleaux::vm::BuiltinId::First);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSeed},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRandomCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kRandomNextUInt64BuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFirstBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2466975172287755897\n");
}

TEST_CASE("VM executes exact-1 Std.Random.Create builtin refs with direct-value integer seeds", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto cSeed = push_u64_const(bytecode_module, 123456789ULL);
  constexpr auto kRandomCreateBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::RandomCreate);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kRandomNextUInt64Builtin = builtin_direct(fleaux::vm::BuiltinId::RandomNextUInt64);
  constexpr auto kFirstBuiltin = builtin_direct(fleaux::vm::BuiltinId::First);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSeed},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kRandomCreateBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRandomNextUInt64Builtin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFirstBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2466975172287755897\n");
}

TEST_CASE("VM executes exact-1 Std.Dict.Length builtin refs with direct dict values", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"answer"}});
  const auto cAnswer = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictLengthBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::DictLength);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAnswer},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kDictLengthBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("VM executes exact-1 Std.Dict.Create clone builtin refs with direct dict values", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"answer"}});
  const auto cAnswer = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictCloneBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::DictCreateDict);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kDictLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictLength);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAnswer},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kDictCloneBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("VM executes Std.Array.GetAt builtin refs through Apply", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kArrayGetAtBuiltinRef = builtin(fleaux::vm::BuiltinId::ArrayGetAt);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kArrayGetAtBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n");
}

TEST_CASE("VM executes Std.Array.Fill builtin refs through Apply", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c7 = push_i64_const(bytecode_module, 7);
  constexpr auto kArrayFillBuiltinRef = builtin(fleaux::vm::BuiltinId::ArrayFill);
  constexpr auto kArrayGetAtBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAt);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kArrayFillBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "7\n");
}

TEST_CASE("VM executes exact-1 Std.Array.Transpose2D builtin refs with direct-value grid arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  constexpr auto kArrayTranspose2DBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ArrayTranspose2D);
  constexpr auto kArrayGetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAtND);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kArrayTranspose2DBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n");
}

TEST_CASE("VM executes Std.Array.ReshapeND builtin refs through Apply", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  constexpr auto kArrayReshapeNDBuiltinRef = builtin(fleaux::vm::BuiltinId::ArrayReshapeND);
  constexpr auto kArrayGetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAtND);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kArrayReshapeNDBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "4\n");
}

TEST_CASE("VM executes Std.Tuple.Map builtin refs through Apply", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kTupleMapBuiltinRef = builtin(fleaux::vm::BuiltinId::TupleMap);
  constexpr auto kToStringBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ToString);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kToStringBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kTupleMapBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n");
}

TEST_CASE("VM executes exact-1 Std.Tuple.Range builtin refs with direct-value integer arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c4 = push_i64_const(bytecode_module, 4);
  constexpr auto kTupleRangeBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::TupleRangeInt64);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kTupleRangeBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0 1 2 3\n");
}

TEST_CASE("VM executes exact-1 Std.Task.Await builtin refs with direct-value task handles", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "AwaitViaApply",
      .link_name = "AwaitViaApply",
      .kind = fleaux::bytecode::ExportKind::kFunction,
      .index = 0,
  });

  fleaux::bytecode::FunctionDef await_via_apply;
  await_via_apply.name = "AwaitViaApply";
  await_via_apply.arity = 1;
  await_via_apply.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef,
       .operand = builtin_direct(fleaux::vm::BuiltinId::TaskAwait)},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Apply)},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(await_via_apply));

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "AwaitViaApply", fleaux::runtime::make_int(7));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::ResultErr(fleaux::runtime::make_tuple(
                          fleaux::runtime::make_string("Task.Await: invalid task handle"))));
}

TEST_CASE("VM executes exact-1 Std.Task.AwaitAll builtin refs with direct-value tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "AwaitAllViaApply",
      .link_name = "AwaitAllViaApply",
      .kind = fleaux::bytecode::ExportKind::kFunction,
      .index = 0,
  });

  fleaux::bytecode::FunctionDef add_one;
  add_one.name = "AddOne";
  add_one.arity = 1;
  add_one.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef await_all_via_apply;
  await_all_via_apply.name = "AwaitAllViaApply";
  await_all_via_apply.arity = 0;
  await_all_via_apply.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::TaskSpawn)},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::TaskSpawn)},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef,
       .operand = builtin_direct(fleaux::vm::BuiltinId::TaskAwaitAll)},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Apply)},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin,
       .operand = builtin_direct(fleaux::vm::BuiltinId::ResultUnwrap)},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(await_all_via_apply));
  bytecode_module.functions.push_back(std::move(add_one));

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto invoked =
      runtime.invoke_symbol(bytecode_module, "AwaitAllViaApply", fleaux::runtime::make_tuple(), output);

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_int(3));
  REQUIRE(output.str() == "2 3\n");
}

TEST_CASE("VM builtin function refs drive TupleReduce through builtin reducers", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kAddBuiltin = builtin(fleaux::vm::BuiltinId::Add);
  constexpr auto kTupleReduceBuiltin = builtin(fleaux::vm::BuiltinId::TupleReduce);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c0 = push_i64_const(bytecode_module, 0);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kAddBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleReduceBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM invoke_symbol dispatches builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "TakeFirst",
      .link_name = "TakeFirst",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.First",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "TakeFirst",
                            fleaux::runtime::make_tuple(fleaux::runtime::make_int(9), fleaux::runtime::make_int(11)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_int(9));
}

TEST_CASE("VM invoke_symbol dispatches Std.Type builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "DescribeType",
      .link_name = "DescribeType",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Type",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "DescribeType",
                            fleaux::runtime::make_tuple(fleaux::runtime::make_int(9), fleaux::runtime::make_int(11)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_string("Tuple"));
}

TEST_CASE("VM invoke_symbol dispatches Std.Cast builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "KeepValue",
      .link_name = "KeepValue",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Cast",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "KeepValue",
                            fleaux::runtime::make_tuple(fleaux::runtime::make_int(9), fleaux::runtime::make_int(11)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_tuple(fleaux::runtime::make_int(9), fleaux::runtime::make_int(11)));
}

TEST_CASE("VM invoke_symbol dispatches Std.ToString builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "DescribeValue",
      .link_name = "DescribeValue",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.ToString",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "DescribeValue",
                            fleaux::runtime::make_tuple(fleaux::runtime::make_int(9), fleaux::runtime::make_int(11)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_string("(9, 11)"));
}

TEST_CASE("VM invoke_symbol dispatches Std.Tuple.Reverse builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "ReverseTuple",
      .link_name = "ReverseTuple",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Tuple.Reverse",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "ReverseTuple",
      fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2), fleaux::runtime::make_int(3)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 3U);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(*invoked, 0)) == 3.0);
}

TEST_CASE("VM invoke_symbol dispatches Std.Tuple.Range builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "RangeByStep",
      .link_name = "RangeByStep",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Tuple.Range#2",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "RangeByStep",
                                       fleaux::runtime::make_tuple(fleaux::runtime::make_int(5),
                                                                   fleaux::runtime::make_int(0),
                                                                   fleaux::runtime::make_int(-2)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 3U);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(*invoked, 2)) == 1.0);
}

TEST_CASE("VM invoke_symbol dispatches Std.Result builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "InspectResultTag",
      .link_name = "InspectResultTag",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Result.Tag",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "InspectResultTag",
                                       fleaux::runtime::ResultOk(fleaux::runtime::make_int(9)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_bool(true));
}

TEST_CASE("VM invoke_symbol dispatches Std.Result.UnwrapErr builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "ExtractResultErr",
      .link_name = "ExtractResultErr",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Result.UnwrapErr",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "ExtractResultErr",
                                       fleaux::runtime::ResultErr(fleaux::runtime::make_string("boom")));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_string("boom"));
}

TEST_CASE("VM invoke_symbol dispatches Std.Random.NextBool builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "StepRandomBool",
      .link_name = "StepRandomBool",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Random.NextBool",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "StepRandomBool",
      fleaux::runtime::RandomCreate(fleaux::runtime::make_tuple(fleaux::runtime::make_uint(123456789ULL))));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::First(*invoked)));
}

TEST_CASE("VM invoke_symbol dispatches Std.Random.Create builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "MakeRandom",
      .link_name = "MakeRandom",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Random.Create",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "MakeRandom", fleaux::runtime::make_uint(123456789ULL));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  const auto stepped = fleaux::runtime::RandomNextUInt64(*invoked);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(stepped))) ==
          "2466975172287755897");
}

TEST_CASE("VM invoke_symbol dispatches Std.Random.Split builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "SplitRandom",
      .link_name = "SplitRandom",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Random.Split",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "SplitRandom",
      fleaux::runtime::RandomCreate(fleaux::runtime::make_tuple(fleaux::runtime::make_uint(123456789ULL))));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  const auto left_step = fleaux::runtime::RandomNextUInt64(fleaux::runtime::First(*invoked));
  const auto right_step = fleaux::runtime::RandomNextUInt64(fleaux::runtime::Second(*invoked));
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(left_step))) ==
          "1278849761381179246");
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(right_step))) ==
          "15883818742529415426");
}

TEST_CASE("VM invoke_symbol dispatches Std.Dict.Keys builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "InspectDictKeys",
      .link_name = "InspectDictKeys",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Dict.Keys",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "InspectDictKeys",
                                       fleaux::runtime::DictSet(fleaux::runtime::make_tuple(
                                           fleaux::runtime::DictCreate_Void(fleaux::runtime::make_tuple()),
                                           fleaux::runtime::make_string("answer"), fleaux::runtime::make_int(42))));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 1U);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::array_at(*invoked, 0)) == "answer");
}

TEST_CASE("VM invoke_symbol dispatches Std.Dict.Contains builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "HasAnswer",
      .link_name = "HasAnswer",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Dict.Contains",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "HasAnswer",
      fleaux::runtime::make_tuple(fleaux::runtime::DictSet(fleaux::runtime::make_tuple(
                                      fleaux::runtime::DictCreate_Void(fleaux::runtime::make_tuple()),
                                      fleaux::runtime::make_string("answer"), fleaux::runtime::make_int(42))),
                                  fleaux::runtime::make_string("answer")));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_bool(true));
}

TEST_CASE("VM invoke_symbol dispatches Std.Array.Slice builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "SliceArray",
      .link_name = "SliceArray",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Array.Slice",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "SliceArray",
                            fleaux::runtime::make_tuple(
                                fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2),
                                                            fleaux::runtime::make_int(3), fleaux::runtime::make_int(4)),
                                fleaux::runtime::make_int(1), fleaux::runtime::make_int(3)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 2U);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(*invoked, 0)) == 2.0);
}

TEST_CASE("VM invoke_symbol dispatches Std.Array.Reshape builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "Gridify",
      .link_name = "Gridify",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Array.Reshape",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "Gridify",
      fleaux::runtime::make_tuple(
          fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2),
                                      fleaux::runtime::make_int(3), fleaux::runtime::make_int(4)),
          fleaux::runtime::make_int(2), fleaux::runtime::make_int(2)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 2U);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(fleaux::runtime::array_at(*invoked, 1), 1)) == 4.0);
}

TEST_CASE("VM invoke_symbol dispatches Std.Array.Transpose2D builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "TransposeGrid",
      .link_name = "TransposeGrid",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Array.Transpose2D",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "TransposeGrid",
      fleaux::runtime::make_tuple(fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2)),
                                  fleaux::runtime::make_tuple(fleaux::runtime::make_int(3), fleaux::runtime::make_int(4))));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(fleaux::runtime::as_array(*invoked).Size() == 2U);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(fleaux::runtime::array_at(*invoked, 1), 0)) == 2.0);
}

TEST_CASE("VM invoke_symbol dispatches Std.Array.GetAtND builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "PickND",
      .link_name = "PickND",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Array.GetAtND",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(
      bytecode_module, "PickND",
      fleaux::runtime::make_tuple(
          fleaux::runtime::make_tuple(fleaux::runtime::make_tuple(fleaux::runtime::make_int(1),
                                                                  fleaux::runtime::make_int(2)),
                                      fleaux::runtime::make_tuple(fleaux::runtime::make_int(3),
                                                                  fleaux::runtime::make_int(4))),
          fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(0))));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_int(3));
}

TEST_CASE("VM invoke_symbol dispatches Std.Task.Await builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "AwaitTask",
      .link_name = "AwaitTask",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Task.Await",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "AwaitTask", fleaux::runtime::make_int(7));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::ResultErr(fleaux::runtime::make_tuple(
                          fleaux::runtime::make_string("Task.Await: invalid task handle"))));
}

TEST_CASE("VM invoke_symbol dispatches Std.Task.Cancel builtin aliases with exact-1 direct-value shape", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "CancelTask",
      .link_name = "CancelTask",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Task.Cancel",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked = runtime.invoke_symbol(bytecode_module, "CancelTask", fleaux::runtime::make_int(7));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::make_bool(false));
}

TEST_CASE("VM invoke_symbol dispatches Std.Task.WithTimeout builtin aliases", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "AwaitTaskWithTimeout",
      .link_name = "AwaitTaskWithTimeout",
      .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = "Std.Task.WithTimeout",
  });

  const fleaux::vm::Runtime runtime;
  auto invoked =
      runtime.invoke_symbol(bytecode_module, "AwaitTaskWithTimeout",
                            fleaux::runtime::make_tuple(fleaux::runtime::make_int(7), fleaux::runtime::make_int(1)));

  if (!invoked.has_value()) {
    INFO(invoked.error().message);
  }
  REQUIRE(invoked.has_value());
  REQUIRE(*invoked == fleaux::runtime::ResultErr(fleaux::runtime::make_tuple(
                          fleaux::runtime::make_string("Task.WithTimeout: invalid task handle"))));
}

TEST_CASE("VM nested callable refs preserve local state across reused invocation scratch", "[vm][closure]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c20 = push_i64_const(bytecode_module, 20);
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  fleaux::bytecode::FunctionDef inner;
  inner.name = "InnerDouble";
  inner.arity = 1;
  inner.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef outer;
  outer.name = "OuterApplyThenAdd";
  outer.arity = 1;
  outer.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(inner));
  bytecode_module.functions.push_back(std::move(outer));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "41\n");
}

TEST_CASE("VM executes native kLoopCall opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c3 = push_i64_const(bytecode_module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCmpGt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(continue_fn));
  bytecode_module.functions.push_back(std::move(step_fn));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kLoopCall, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0\n");
}

TEST_CASE("VM executes native kLoopNCall opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c10 = push_i64_const(bytecode_module, 10);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCmpGt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(continue_fn));
  bytecode_module.functions.push_back(std::move(step_fn));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kLoopNCall, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0\n");
}

TEST_CASE("VM reports native kLoopNCall max-iteration failure", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c3 = push_i64_const(bytecode_module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCmpGt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(continue_fn));
  bytecode_module.functions.push_back(std::move(step_fn));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kLoopNCall, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "native 'loop_n_call' threw: LoopN: exceeded max_iters");
}

TEST_CASE("VM kCallBuiltin executes Std.Loop and Std.LoopN through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c3 = push_i64_const(bytecode_module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCmpGt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(continue_fn));
  bytecode_module.functions.push_back(std::move(step_fn));
  constexpr auto kLoopBuiltin = builtin(fleaux::vm::BuiltinId::Loop);
  constexpr auto kLoopNBuiltin = builtin(fleaux::vm::BuiltinId::LoopN);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLoopBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLoopNBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0\n0\n");
}

TEST_CASE("VM kCallBuiltin reports Std.LoopN errors through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c3 = push_i64_const(bytecode_module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCmpGt, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(continue_fn));
  bytecode_module.functions.push_back(std::move(step_fn));
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::LoopN)},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "builtin 'Std.LoopN' threw: LoopN: exceeded max_iters");
}

TEST_CASE("VM reports missing halt", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "program terminated without halt");
}

TEST_CASE("VM native Float64 kDiv by zero returns infinity", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_f64_const(bytecode_module, 10.0);
  const auto c0 = push_f64_const(bytecode_module, 0.0);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());

  std::string lowered = output.str();
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  REQUIRE(lowered.find("inf") != std::string::npos);
}

TEST_CASE("VM native Float64 kMod by zero returns NaN", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_f64_const(bytecode_module, 10.0);
  const auto c0 = push_f64_const(bytecode_module, 0.0);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kMod, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());

  std::string lowered = output.str();
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  REQUIRE(lowered.find("nan") != std::string::npos);
}

TEST_CASE("VM native NaN is not equal to itself", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c0 = push_f64_const(bytecode_module, 0.0);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},  // NaN
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCmpEq, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "False\n");
}

TEST_CASE("VM native integer arithmetic fails on overflow and division by zero", "[vm]") {
  const auto run_binary = [](const fleaux::bytecode::Opcode opcode, const fleaux::bytecode::ConstValue& lhs,
                             const fleaux::bytecode::ConstValue& rhs) {
    fleaux::bytecode::Module bytecode_module;
    bytecode_module.constants = {lhs, rhs};
    bytecode_module.instructions = {
        {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
        {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 1},
        {.opcode = opcode, .operand = 0},
        {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
    };
    std::ostringstream output;
    const fleaux::vm::Runtime runtime;
    return runtime.execute(bytecode_module, output);
  };
  const auto expect_failure = [&](const fleaux::bytecode::Opcode opcode, const fleaux::bytecode::ConstValue& lhs,
                                  const fleaux::bytecode::ConstValue& rhs, const std::string& message) {
    const auto result = run_binary(opcode, lhs, rhs);
    REQUIRE_FALSE(result.has_value());
    INFO("error: " << result.error().message);
    REQUIRE(result.error().message.find(message) != std::string::npos);
  };

  constexpr auto kInt64Max = std::numeric_limits<std::int64_t>::max();
  constexpr auto kInt64Min = std::numeric_limits<std::int64_t>::min();
  expect_failure(fleaux::bytecode::Opcode::kAdd, {kInt64Max}, {std::int64_t{1}}, "Add: Int64 overflow");
  expect_failure(fleaux::bytecode::Opcode::kSub, {kInt64Min}, {std::int64_t{1}}, "Subtract: Int64 overflow");
  expect_failure(fleaux::bytecode::Opcode::kMul, {kInt64Max}, {std::int64_t{2}}, "Multiply: Int64 overflow");
  expect_failure(fleaux::bytecode::Opcode::kSub, {std::uint64_t{1}}, {std::uint64_t{2}}, "Subtract: UInt64 overflow");
  expect_failure(fleaux::bytecode::Opcode::kDiv, {std::int64_t{1}}, {std::int64_t{0}}, "Divide: Int64 division by zero");
  expect_failure(fleaux::bytecode::Opcode::kDiv, {kInt64Min}, {std::int64_t{-1}}, "Divide: Int64 overflow");
  expect_failure(fleaux::bytecode::Opcode::kMod, {std::uint64_t{1}}, {std::uint64_t{0}}, "Mod: UInt64 division by zero");
  expect_failure(fleaux::bytecode::Opcode::kPow, {std::int64_t{2}}, {std::int64_t{-1}},
                 "Pow: Int64 exponent must not be negative");
  expect_failure(fleaux::bytecode::Opcode::kPow, {std::int64_t{2}}, {std::int64_t{64}}, "Pow: Int64 overflow");
}

TEST_CASE("VM native integer arithmetic is exact and keeps the operand type", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  constexpr auto kTypeBuiltin = builtin(fleaux::vm::BuiltinId::Type);
  const auto big = push_i64_const(bytecode_module, 9007199254740993);  // 2^53 + 1, not representable as double
  const auto zero = push_i64_const(bytecode_module, 0);
  const auto seven = push_i64_const(bytecode_module, -7);
  const auto two = push_i64_const(bytecode_module, 2);
  const auto ten_u = push_u64_const(bytecode_module, 10U);
  const auto four_u = push_u64_const(bytecode_module, 4U);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = big},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = zero},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = seven},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = two},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = seven},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = two},
      {.opcode = fleaux::bytecode::Opcode::kMod, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = ten_u},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = four_u},
      {.opcode = fleaux::bytecode::Opcode::kDiv, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTypeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9007199254740993\n-3\n-1\nUInt64\n");
}

TEST_CASE("VM kPushConst preserves UInt64 constants", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kTypeBuiltin = builtin(fleaux::vm::BuiltinId::Type);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c42u = push_u64_const(bytecode_module, 42U);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42u},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTypeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "UInt64\n");
}

TEST_CASE("VM native kAdd preserves UInt64 for UInt64 operands", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kTypeBuiltin = builtin(fleaux::vm::BuiltinId::Type);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c40u = push_u64_const(bytecode_module, 40U);
  const auto c2u = push_u64_const(bytecode_module, 2U);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c40u},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2u},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTypeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "UInt64\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Add through primary builtin dispatch and preserves UInt64 results", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c5 = push_i64_const(bytecode_module, 5);
  const auto u40 = push_u64_const(bytecode_module, 40);
  const auto u2 = push_u64_const(bytecode_module, 2);
  constexpr auto kAddBuiltin = builtin(fleaux::vm::BuiltinId::Add);
  constexpr auto kTypeBuiltin = builtin(fleaux::vm::BuiltinId::Type);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kAddBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = u40},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = u2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kAddBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTypeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\nUInt64\n");
}

TEST_CASE("VM kCallBuiltin resolves Std.Println via builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin executes Std.Println through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin executes comparison and logical builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto cNeg1 = push_i64_const(bytecode_module, -1);
  const auto u2 = push_u64_const(bytecode_module, 2);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto cTrue = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto cFalse = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  constexpr auto kLessThanBuiltin = builtin(fleaux::vm::BuiltinId::LessThan);
  constexpr auto kAndBuiltin = builtin(fleaux::vm::BuiltinId::And);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cNeg1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = u2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLessThanBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrue},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFalse},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kAndBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\nFalse\n");
}

TEST_CASE("VM kCallBuiltin executes unary builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  constexpr auto kUnaryMinusBuiltin = builtin(fleaux::vm::BuiltinId::UnaryMinus);
  constexpr auto kUnaryPlusBuiltin = builtin(fleaux::vm::BuiltinId::UnaryPlus);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kUnaryMinusBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kUnaryPlusBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "-10\n10\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Select through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  constexpr auto kSelectBuiltin = builtin(fleaux::vm::BuiltinId::Select);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSelectBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "20\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Branch through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c10 = push_i64_const(bytecode_module, 10);

  fleaux::bytecode::FunctionDef add1;
  add1.name = "AddOne";
  add1.arity = 1;
  add1.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef sub1;
  sub1.name = "SubOne";
  sub1.arity = 1;
  sub1.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kSub, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(add1));
  bytecode_module.functions.push_back(std::move(sub1));
  constexpr auto kBranchBuiltin = builtin(fleaux::vm::BuiltinId::Branch);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kBranchBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "11\n");
}

TEST_CASE("VM executes native mul/neg/comparison/or opcodes", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c6 = push_i64_const(bytecode_module, 6);
  const auto c7 = push_i64_const(bytecode_module, 7);
  const auto c3 = push_i64_const(bytecode_module, 3);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kMul, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kNeg, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kCmpNe, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kCmpLe, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kCmpGe, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kOr, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n-3\nTrue\nTrue\nTrue\nTrue\n");
}

TEST_CASE("VM executes kCallUserFunc opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c21 = push_i64_const(bytecode_module, 21);

  fleaux::bytecode::FunctionDef twice;
  twice.name = "Twice";
  twice.arity = 1;
  twice.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(twice));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c21},
      {.opcode = fleaux::bytecode::Opcode::kCallUserFunc, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallUserFunc preserves tuple-valued direct arguments for unary functions", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c21 = push_i64_const(bytecode_module, 21);

  fleaux::bytecode::FunctionDef inspect;
  inspect.name = "InspectTuple";
  inspect.arity = 1;
  inspect.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(inspect));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c21},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallUserFunc, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("VM kCallUserFunc binds fixed-arity tuple arguments", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c32 = push_i64_const(bytecode_module, 32);

  fleaux::bytecode::FunctionDef sum2;
  sum2.name = "Sum2";
  sum2.arity = 2;
  sum2.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(sum2));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c32},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallUserFunc, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallUserFuncBinary binds binary arguments directly", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c32 = push_i64_const(bytecode_module, 32);

  fleaux::bytecode::FunctionDef sum2;
  sum2.name = "Sum2";
  sum2.arity = 2;
  sum2.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(sum2));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c32},
      {.opcode = fleaux::bytecode::Opcode::kCallUserFuncBinary, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallUserFunc binds variadic tail from owned tuple arguments", "[vm][variadic]") {
  fleaux::bytecode::Module bytecode_module;
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);
  const auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  const auto c30 = push_i64_const(bytecode_module, 30);

  fleaux::bytecode::FunctionDef count_tail;
  count_tail.name = "CountTail";
  count_tail.arity = 2;
  count_tail.has_variadic_tail = true;
  count_tail.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(count_tail));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallUserFunc, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Apply through primary builtin dispatch for inline closure callables", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c32 = push_i64_const(bytecode_module, 32);

  fleaux::bytecode::FunctionDef closure_fn;
  closure_fn.name = "__closure_add_capture";
  closure_fn.arity = 2;
  closure_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(closure_fn));
  bytecode_module.closures.push_back(fleaux::bytecode::ClosureDef{
      .function_index = 0,
      .capture_count = 1,
      .declared_arity = 1,
      .declared_has_variadic_tail = false,
  });
  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c32},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallBuiltin executes captured-only inline closures without tuple-wrapping the final argument",
          "[vm][closure]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);

  fleaux::bytecode::FunctionDef closure_fn;
  closure_fn.name = "__closure_capture_only";
  closure_fn.arity = 1;
  closure_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(closure_fn));
  bytecode_module.closures.push_back(fleaux::bytecode::ClosureDef{
      .function_index = 0,
      .capture_count = 1,
      .declared_arity = 0,
      .declared_has_variadic_tail = false,
  });

  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallBuiltin preserves capture-prefix order for inline closures", "[vm][closure]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c7 = push_i64_const(bytecode_module, 7);
  const auto c11 = push_i64_const(bytecode_module, 11);
  const auto c24 = push_i64_const(bytecode_module, 24);

  fleaux::bytecode::FunctionDef closure_fn;
  closure_fn.name = "__closure_capture_prefix_order";
  closure_fn.arity = 3;
  closure_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(closure_fn));
  bytecode_module.closures.push_back(fleaux::bytecode::ClosureDef{
      .function_index = 0,
      .capture_count = 2,
      .declared_arity = 1,
      .declared_has_variadic_tail = false,
  });

  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c24},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallBuiltin preserves captured prefix and variadic tail for inline closures",
          "[vm][closure][variadic]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c5 = push_i64_const(bytecode_module, 5);
  const auto c7 = push_i64_const(bytecode_module, 7);
  const auto c11 = push_i64_const(bytecode_module, 11);
  const auto c13 = push_i64_const(bytecode_module, 13);
  const auto c0 = push_i64_const(bytecode_module, 0);

  fleaux::bytecode::FunctionDef closure_fn;
  closure_fn.name = "__closure_capture_variadic_tail";
  closure_fn.arity = 3;
  closure_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::ElementAt)},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(closure_fn));
  bytecode_module.closures.push_back(fleaux::bytecode::ClosureDef{
      .function_index = 0,
      .capture_count = 1,
      .declared_arity = 2,
      .declared_has_variadic_tail = true,
  });

  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c13},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "23\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Apply for two-argument inline closure callables", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c32 = push_i64_const(bytecode_module, 32);

  fleaux::bytecode::FunctionDef sum2;
  sum2.name = "__closure_sum2_ok";
  sum2.arity = 2;
  sum2.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(sum2));
  bytecode_module.closures.push_back(fleaux::bytecode::ClosureDef{
      .function_index = 0,
      .capture_count = 0,
      .declared_arity = 2,
      .declared_has_variadic_tail = false,
  });

  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c32},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Match with wildcard closure case through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c0 = push_i64_const(bytecode_module, 0);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"zero"}});
  const auto cZero = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"many"}});
  const auto cMany = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"__fleaux_match_wildcard__"}});
  const auto cWildcard = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  fleaux::bytecode::FunctionDef zero_fn;
  zero_fn.name = "CaseZero";
  zero_fn.arity = 0;
  zero_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cZero},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef many_fn;
  many_fn.name = "CaseMany";
  many_fn.arity = 0;
  many_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMany},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(zero_fn));
  bytecode_module.functions.push_back(std::move(many_fn));
  constexpr auto kMatchBuiltin = builtin(fleaux::vm::BuiltinId::Match);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cWildcard},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kMatchBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "many\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Match with predicate pattern case through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c6 = push_i64_const(bytecode_module, 6);
  const auto c2 = push_i64_const(bytecode_module, 2);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"even"}});
  const auto cEven = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"odd"}});
  const auto cOdd = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"__fleaux_match_wildcard__"}});
  const auto cWildcard = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  fleaux::bytecode::FunctionDef is_even_fn;
  is_even_fn.name = "IsEven";
  is_even_fn.arity = 1;
  is_even_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kMod, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kMod, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCmpEq, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef even_fn;
  even_fn.name = "CaseEven";
  even_fn.arity = 1;
  even_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEven},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  fleaux::bytecode::FunctionDef odd_fn;
  odd_fn.name = "CaseOdd";
  odd_fn.arity = 1;
  odd_fn.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cOdd},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };

  bytecode_module.functions.push_back(std::move(is_even_fn));
  bytecode_module.functions.push_back(std::move(even_fn));
  bytecode_module.functions.push_back(std::move(odd_fn));
  constexpr auto kMatchBuiltin = builtin(fleaux::vm::BuiltinId::Match);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},

      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cWildcard},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kMatchBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "even\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Result builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"boom"}});
  const auto cBoom = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kResultOkBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultOk);
  constexpr auto kResultIsOkBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultIsOk);
  constexpr auto kResultUnwrapBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultUnwrap);
  constexpr auto kResultErrBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultErr);
  constexpr auto kResultIsErrBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultIsErr);
  constexpr auto kResultUnwrapErrBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultUnwrapErr);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultIsOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultUnwrapBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBoom},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultIsErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultUnwrapErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n42\nTrue\nboom\n");
}

TEST_CASE(
    "VM kCallBuiltin executes Std.Random builtins through primary builtin dispatch with exact-1 direct-value shape",
    "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto cSeed = push_u64_const(bytecode_module, 123456789ULL);
  constexpr auto kRandomCreateBuiltin = builtin_direct(fleaux::vm::BuiltinId::RandomCreate);
  constexpr auto kRandomSplitBuiltin = builtin_direct(fleaux::vm::BuiltinId::RandomSplit);
  constexpr auto kFirstBuiltin = builtin_direct(fleaux::vm::BuiltinId::First);
  constexpr auto kRandomNextUInt64Builtin = builtin_direct(fleaux::vm::BuiltinId::RandomNextUInt64);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSeed},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRandomCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRandomSplitBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFirstBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRandomNextUInt64Builtin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFirstBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1278849761381179246\n");
}

TEST_CASE(
    "VM kCallBuiltin executes unary Std.Dict builtins through primary builtin dispatch with exact-1 direct-value shape",
    "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c9 = push_i64_const(bytecode_module, 9);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictLength);
  constexpr auto kDictKeysBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictKeys);
  constexpr auto kDictClearBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictClear);
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictKeysBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictClearBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1\n1\n0\n");
}

TEST_CASE(
    "VM kCallBuiltin executes unary Std.Tuple builtins through primary builtin dispatch with exact-1 direct-value shape",
    "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kTupleReverseBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleReverse);
  constexpr auto kTupleSortBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleSort);
  constexpr auto kTupleUniqueBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleUnique);
  constexpr auto kTupleMinBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleMin);
  constexpr auto kTupleMaxBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleMax);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleReverseBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleSortBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleUniqueBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleMinBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleMaxBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3 2 1\n1 2 3\n3 1 2\n1\n3\n");
}

TEST_CASE("VM kCallBuiltin executes higher-order Std.Tuple builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  constexpr auto kTupleMapBuiltin = builtin(fleaux::vm::BuiltinId::TupleMap);
  constexpr auto kTupleFilterBuiltin = builtin(fleaux::vm::BuiltinId::TupleFilter);
  constexpr auto kTupleReduceBuiltin = builtin(fleaux::vm::BuiltinId::TupleReduce);
  constexpr auto kTupleFindIndexBuiltin = builtin(fleaux::vm::BuiltinId::TupleFindIndex);
  constexpr auto kTupleAnyBuiltin = builtin(fleaux::vm::BuiltinId::TupleAny);
  constexpr auto kTupleAllBuiltin = builtin(fleaux::vm::BuiltinId::TupleAll);
  constexpr auto kToStringBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ToString);
  constexpr auto kResultIsOkBuiltinRef = builtin_direct(fleaux::vm::BuiltinId::ResultIsOk);
  constexpr auto kResultOkBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultOk);
  constexpr auto kResultErrBuiltin = builtin_direct(fleaux::vm::BuiltinId::ResultErr);
  constexpr auto kAddBuiltinRef = builtin(fleaux::vm::BuiltinId::Add);
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kToStringBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleMapBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultIsOkBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleFilterBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultIsOkBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleFindIndexBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultErrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultIsOkBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleAnyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kResultOkBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kResultIsOkBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleAllBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = kAddBuiltinRef},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleReduceBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n2\n1\nTrue\nTrue\n6\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Tuple.Range overloads through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto cNeg2 = push_i64_const(bytecode_module, -2);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c5 = push_i64_const(bytecode_module, 5);
  constexpr auto kTupleRangeUnaryBuiltin = builtin_direct(fleaux::vm::BuiltinId::TupleRangeInt64);
  constexpr auto kTupleRangeBinaryBuiltin = builtin(fleaux::vm::BuiltinId::TupleRangeInt64Int64);
  constexpr auto kTupleRangeTernaryBuiltin = builtin(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64);
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kElementAtBuiltin = builtin(fleaux::vm::BuiltinId::ElementAt);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleRangeUnaryBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleRangeBinaryBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kElementAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cNeg2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleRangeTernaryBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kElementAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "4\n2\n1\n");
}

TEST_CASE("VM kCallBuiltin executes multi-argument Std.Dict builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c9 = push_i64_const(bytecode_module, 9);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"a"}});
  const auto cA = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"b"}});
  const auto cB = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"missing"}});
  const auto cMissing = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictCloneBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictCreateDict);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictContainsBuiltin = builtin(fleaux::vm::BuiltinId::DictContains);
  constexpr auto kDictGetDefaultBuiltin = builtin(fleaux::vm::BuiltinId::DictGetDefault);
  constexpr auto kDictMergeBuiltin = builtin(fleaux::vm::BuiltinId::DictMerge);
  constexpr auto kDictDeleteBuiltin = builtin(fleaux::vm::BuiltinId::DictDelete);
  constexpr auto kDictLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictLength);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cA},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCloneBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cA},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictContainsBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMissing},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetDefaultBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cB},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictMergeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cA},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictDeleteBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n9\n2\n1\n");
}

TEST_CASE("VM kCallBuiltin executes 1-D Std.Array builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c9 = push_i64_const(bytecode_module, 9);
  constexpr auto kArraySetAtBuiltin = builtin(fleaux::vm::BuiltinId::ArraySetAt);
  constexpr auto kArrayGetAtBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAt);
  constexpr auto kArraySliceBuiltin = builtin(fleaux::vm::BuiltinId::ArraySlice);
  constexpr auto kArrayConcatBuiltin = builtin(fleaux::vm::BuiltinId::ArrayConcat);
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArraySetAtBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArraySliceBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayConcatBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\n3\n");
}

TEST_CASE("VM kCallBuiltin executes structural Std.Array builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c7 = push_i64_const(bytecode_module, 7);
  const auto c9 = push_i64_const(bytecode_module, 9);
  constexpr auto kArraySetAt2DBuiltin = builtin(fleaux::vm::BuiltinId::ArraySetAt2D);
  constexpr auto kArrayFillBuiltin = builtin(fleaux::vm::BuiltinId::ArrayFill);
  constexpr auto kArraySlice2DBuiltin = builtin(fleaux::vm::BuiltinId::ArraySlice2D);
  constexpr auto kArrayReshapeBuiltin = builtin(fleaux::vm::BuiltinId::ArrayReshape);
  constexpr auto kArrayGetAtBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAt);
  constexpr auto kArrayGetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAtND);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArraySetAt2DBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayFillBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArraySlice2DBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayReshapeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\n7\n4\n4\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Array.Transpose2D through primary builtin dispatch with exact-1 direct-value shape",
          "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  constexpr auto kArrayTranspose2DBuiltin = builtin_direct(fleaux::vm::BuiltinId::ArrayTranspose2D);
  constexpr auto kArrayGetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAtND);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayTranspose2DBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n");
}

TEST_CASE("VM kCallBuiltin executes n-D Std.Array builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c5 = push_i64_const(bytecode_module, 5);
  const auto c6 = push_i64_const(bytecode_module, 6);
  const auto c7 = push_i64_const(bytecode_module, 7);
  const auto c8 = push_i64_const(bytecode_module, 8);
  const auto c70 = push_i64_const(bytecode_module, 70);
  constexpr auto kArrayGetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayGetAtND);
  constexpr auto kArraySetAtNDBuiltin = builtin(fleaux::vm::BuiltinId::ArraySetAtND);
  constexpr auto kArrayReshapeNDBuiltin = builtin(fleaux::vm::BuiltinId::ArrayReshapeND);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c70},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArraySetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 8},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayReshapeNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kArrayGetAtNDBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "6\n70\n8\n");
}

TEST_CASE("VM kCallBuiltin executes more arithmetic/logical builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c7 = push_i64_const(bytecode_module, 7);

  constexpr auto kSubtractBuiltin = builtin(fleaux::vm::BuiltinId::Subtract);
  constexpr auto kMultiplyBuiltin = builtin(fleaux::vm::BuiltinId::Multiply);
  constexpr auto kOrBuiltin = builtin(fleaux::vm::BuiltinId::Or);
  constexpr auto kEqualBuiltin = builtin(fleaux::vm::BuiltinId::Equal);
  constexpr auto kNotEqualBuiltin = builtin(fleaux::vm::BuiltinId::NotEqual);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSubtractBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kMultiplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOrBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kEqualBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kNotEqualBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "6\n21\nTrue\nTrue\nTrue\n");
}

TEST_CASE("VM kCallBuiltin executes bitwise builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c6 = push_i64_const(bytecode_module, 6);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c12 = push_i64_const(bytecode_module, 12);
  const auto c2 = push_i64_const(bytecode_module, 2);

  constexpr auto kBitAndBuiltin = builtin(fleaux::vm::BuiltinId::BitAnd);
  constexpr auto kBitNotBuiltin = builtin(fleaux::vm::BuiltinId::BitNot);
  constexpr auto kBitShiftLeftBuiltin = builtin(fleaux::vm::BuiltinId::BitShiftLeft);
  constexpr auto kBitShiftRightBuiltin = builtin(fleaux::vm::BuiltinId::BitShiftRight);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kBitAndBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kBitNotBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kBitShiftLeftBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c12},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kBitShiftRightBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n-1\n12\n3\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Type through direct unary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin_direct(fleaux::vm::BuiltinId::Type)},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "Tuple\n");
}
TEST_CASE("VM kCallBuiltin preserves empty-tuple runtime failures for Std.Tuple.Min", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin_direct(fleaux::vm::BuiltinId::TupleMin)},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "builtin 'Std.Tuple.Min' threw: TupleMin expects non-empty tuple");
}
TEST_CASE("VM kCallBuiltin preserves ragged-grid runtime failures for Std.Array.Transpose2D", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin,
       .operand = builtin_direct(fleaux::vm::BuiltinId::ArrayTranspose2D)},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message ==
          "builtin 'Std.Array.Transpose2D' threw: ArrayTranspose2D: ragged grid detected at row 1");
}
TEST_CASE("VM kCallBuiltin preserves step-zero runtime failures for Std.Tuple.Range", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c5 = push_i64_const(bytecode_module, 5);
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin,
       .operand = builtin(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64)},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "builtin 'Std.Tuple.Range' threw: TupleRange step cannot be 0");
}

TEST_CASE("VM kCallBuiltin executes apply, wrap, unwrap, cast, and to_num through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c41 = push_i64_const(bytecode_module, 41);
  const auto c7 = push_i64_const(bytecode_module, 7);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"42"}});

  fleaux::bytecode::FunctionDef add_one;
  add_one.name = "AddOne";
  add_one.arity = 1;
  add_one.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(add_one));

  constexpr auto kApplyBuiltin = builtin(fleaux::vm::BuiltinId::Apply);
  constexpr auto kWrapBuiltin = builtin(fleaux::vm::BuiltinId::Wrap);
  constexpr auto kUnwrapBuiltin = builtin(fleaux::vm::BuiltinId::Unwrap);
  constexpr auto kCastBuiltin = builtin(fleaux::vm::BuiltinId::Cast);
  constexpr auto kToNumBuiltin = builtin(fleaux::vm::BuiltinId::ToNum);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c41},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kApplyBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kWrapBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kUnwrapBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kCastBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kToNumBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n7\n42\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Cast through primary builtin dispatch with exact-1 direct-value shape",
          "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c11 = push_i64_const(bytecode_module, 11);
  constexpr auto kCastBuiltin = builtin_direct(fleaux::vm::BuiltinId::Cast);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c11},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kCastBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9 11\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Task.AwaitAll through primary builtin dispatch with exact-1 direct-value shape",
          "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);

  fleaux::bytecode::FunctionDef add_one;
  add_one.name = "AddOne";
  add_one.arity = 1;
  add_one.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kLoadLocal, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kReturn, .operand = 0},
  };
  bytecode_module.functions.push_back(std::move(add_one));

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::TaskSpawn)},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::TaskSpawn)},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin,
       .operand = builtin_direct(fleaux::vm::BuiltinId::TaskAwaitAll)},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin,
       .operand = builtin_direct(fleaux::vm::BuiltinId::ResultUnwrap)},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = builtin(fleaux::vm::BuiltinId::Println)},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2 3\n3\n");
}

TEST_CASE("VM kCallBuiltin executes numeric cast helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto cInt42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{3.0});
  const auto cFloatThree = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{7.0});
  const auto cFloatSeven = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kToIntBuiltin = builtin(fleaux::vm::BuiltinId::ToInt64);
  constexpr auto kToUIntBuiltin = builtin(fleaux::vm::BuiltinId::ToUInt64);
  constexpr auto kToFloatBuiltin = builtin(fleaux::vm::BuiltinId::ToFloat64);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFloatThree},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kToIntBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cInt42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kToUIntBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFloatSeven},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kToFloatBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n42\n7\n");
}

TEST_CASE("VM kCallBuiltin executes core sequence helpers and math helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  const auto c30 = push_i64_const(bytecode_module, 30);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c5 = push_i64_const(bytecode_module, 5);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{2.9});
  const auto cFloatTwoPointNine = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{2.1});
  const auto cFloatTwoPointOne = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{-7.5});
  const auto cFloatNegativeSevenPointFive = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kElementAtBuiltin = builtin(fleaux::vm::BuiltinId::ElementAt);
  constexpr auto kTakeBuiltin = builtin(fleaux::vm::BuiltinId::Take);
  constexpr auto kDropBuiltin = builtin(fleaux::vm::BuiltinId::Drop);
  constexpr auto kSliceStopBuiltin = builtin(fleaux::vm::BuiltinId::SliceTupleUInt64);
  constexpr auto kSliceStartStopBuiltin = builtin(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64);
  constexpr auto kSliceStartStopStepBuiltin = builtin(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64);
  constexpr auto kSqrtBuiltin = builtin(fleaux::vm::BuiltinId::Sqrt);
  constexpr auto kSinBuiltin = builtin(fleaux::vm::BuiltinId::Sin);
  constexpr auto kCosBuiltin = builtin(fleaux::vm::BuiltinId::Cos);
  constexpr auto kTanBuiltin = builtin(fleaux::vm::BuiltinId::Tan);
  constexpr auto kFloorBuiltin = builtin(fleaux::vm::BuiltinId::MathFloor);
  constexpr auto kCeilBuiltin = builtin(fleaux::vm::BuiltinId::MathCeil);
  constexpr auto kAbsBuiltin = builtin(fleaux::vm::BuiltinId::MathAbs);
  constexpr auto kLogBuiltin = builtin(fleaux::vm::BuiltinId::MathLog);
  constexpr auto kClampBuiltin = builtin(fleaux::vm::BuiltinId::MathClamp);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      // Std.Length: pass the 3-tuple directly (no 1-element wrapper).
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kElementAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTakeBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDropBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSliceStartStopBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSliceStopBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSliceStartStopStepBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSqrtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kSinBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kCosBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTanBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFloatTwoPointNine},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFloorBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFloatTwoPointOne},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kCeilBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFloatNegativeSevenPointFive},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kAbsBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLogBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kClampBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n20\n10 20\n30\n20 30\n10 20\n10 30\n3\n0\n1\n0\n2\n3\n7.5\n0\n5\n");
}

TEST_CASE("VM kCallBuiltin executes Std.ToString and Std.String helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c5 = push_i64_const(bytecode_module, 5);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"  +42"}});
  const auto cParseInt = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"42"}});
  const auto cParseUInt = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{" +6.25e1"}});
  const auto cParseFloat = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"hElLo"}});
  const auto cHello = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"MiXeD"}});
  const auto cMixed = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"  trim me  "}});
  const auto cTrim = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"a,b,c"}});
  const auto cCsv = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{","}});
  const auto cComma = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"-"}});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"_"}});
  const auto cUnderscore = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc"}});
  const auto cAbc = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"ab"}});
  const auto cAb = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"bc"}});
  const auto cBc = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"b"}});
  const auto cB = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abcd"}});
  const auto cAbcd = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"  left"}});
  const auto cTrimStartOnly = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"right  "}});
  const auto cTrimEndOnly = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abcdef"}});
  const auto cAbcdef = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abcabc"}});
  const auto cAbcabc = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"{} + {} = {}"}});
  const auto cFormat = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kToStringBuiltin = builtin(fleaux::vm::BuiltinId::ToString);
  constexpr auto kStringParseInt64Builtin = builtin_direct(fleaux::vm::BuiltinId::StringParseInt64);
  constexpr auto kStringParseUInt64Builtin = builtin_direct(fleaux::vm::BuiltinId::StringParseUInt64);
  constexpr auto kStringParseFloat64Builtin = builtin_direct(fleaux::vm::BuiltinId::StringParseFloat64);
  constexpr auto kStringUpperBuiltin = builtin(fleaux::vm::BuiltinId::StringUpper);
  constexpr auto kStringLowerBuiltin = builtin(fleaux::vm::BuiltinId::StringLower);
  constexpr auto kStringTrimBuiltin = builtin(fleaux::vm::BuiltinId::StringTrim);
  constexpr auto kStringSplitBuiltin = builtin(fleaux::vm::BuiltinId::StringSplit);
  constexpr auto kStringJoinBuiltin = builtin(fleaux::vm::BuiltinId::StringJoin);
  constexpr auto kStringReplaceBuiltin = builtin(fleaux::vm::BuiltinId::StringReplace);
  constexpr auto kStringContainsBuiltin = builtin(fleaux::vm::BuiltinId::StringContains);
  constexpr auto kStringStartsWithBuiltin = builtin(fleaux::vm::BuiltinId::StringStartsWith);
  constexpr auto kStringEndsWithBuiltin = builtin(fleaux::vm::BuiltinId::StringEndsWith);
  constexpr auto kStringLengthBuiltin = builtin(fleaux::vm::BuiltinId::StringLength);
  constexpr auto kStringTrimStartBuiltin = builtin(fleaux::vm::BuiltinId::StringTrimStart);
  constexpr auto kStringTrimEndBuiltin = builtin(fleaux::vm::BuiltinId::StringTrimEnd);
  constexpr auto kStringCharAtBuiltin = builtin(fleaux::vm::BuiltinId::StringCharAt);
  constexpr auto kStringSliceBuiltin = builtin(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64);
  constexpr auto kStringFindBuiltin = builtin(fleaux::vm::BuiltinId::StringFindStringStringUInt64);
  constexpr auto kStringFormatBuiltin = builtin(fleaux::vm::BuiltinId::StringFormat);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kToStringBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cParseInt},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringParseInt64Builtin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cParseUInt},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringParseUInt64Builtin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cParseFloat},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringParseFloat64Builtin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cHello},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringUpperBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMixed},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringLowerBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrim},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringTrimBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cCsv},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringSplitBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cB},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBc},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringJoinBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cCsv},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cUnderscore},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringReplaceBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cB},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringContainsBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAb},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringStartsWithBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBc},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringEndsWithBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbcd},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrimStartOnly},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringTrimStartBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrimEndOnly},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringTrimEndBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringCharAtBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbcdef},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringSliceBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbcabc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringFindBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFormat},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringFormatBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() ==
          "42\n42\n42\n62.5\nHELLO\nmixed\ntrim me\na b "
          "c\nabc,b,bc\na_b_c\nTrue\nTrue\nTrue\n4\nleft\nright\nb\nbcd\n4\n2 + 3 = 5\n");
}

TEST_CASE("VM kCallBuiltin executes Std.String.Regex helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc-123 xyz"}});
  const auto cText = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"[a-z]+-[0-9]+"}});
  const auto cWholePattern = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"[0-9]+"}});
  const auto cDigitsPattern = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"one,two;three"}});
  const auto cSplitText = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"[,;]"}});
  const auto cSplitPattern = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"|"}});
  const auto cPipe = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kRegexIsMatchBuiltin = builtin(fleaux::vm::BuiltinId::StringRegexIsMatch);
  constexpr auto kRegexFindBuiltin = builtin(fleaux::vm::BuiltinId::StringRegexFind);
  constexpr auto kRegexReplaceBuiltin = builtin(fleaux::vm::BuiltinId::StringRegexReplace);
  constexpr auto kRegexSplitBuiltin = builtin(fleaux::vm::BuiltinId::StringRegexSplit);
  constexpr auto kLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::Length);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cText},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cWholePattern},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRegexIsMatchBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cText},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDigitsPattern},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRegexFindBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSplitText},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSplitPattern},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cPipe},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRegexReplaceBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSplitText},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cSplitPattern},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kRegexSplitBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n4\none|two|three\n3\n");
}

TEST_CASE("VM kCallBuiltin executes Std.Path and Std.OS helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp"}});
  const auto cTmp = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"file.txt"}});
  const auto cFile = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp/file.txt"}});
  const auto cFull = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"."}});
  const auto cDot = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"log"}});
  const auto cLogExt = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"other.bin"}});
  const auto cOtherBin = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kPathJoinBuiltin = builtin(fleaux::vm::BuiltinId::PathJoin);
  constexpr auto kPathBasenameBuiltin = builtin(fleaux::vm::BuiltinId::PathBasename);
  constexpr auto kPathExtensionBuiltin = builtin(fleaux::vm::BuiltinId::PathExtension);
  constexpr auto kPathStemBuiltin = builtin(fleaux::vm::BuiltinId::PathStem);
  constexpr auto kPathExistsBuiltin = builtin(fleaux::vm::BuiltinId::PathExists);
  constexpr auto kPathIsDirBuiltin = builtin(fleaux::vm::BuiltinId::PathIsDir);
  constexpr auto kOSIsLinuxBuiltin = builtin(fleaux::vm::BuiltinId::OSIsLinux);
  constexpr auto kPathWithExtensionBuiltin = builtin(fleaux::vm::BuiltinId::PathWithExtension);
  constexpr auto kPathWithBasenameBuiltin = builtin(fleaux::vm::BuiltinId::PathWithBasename);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTmp},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathJoinBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathBasenameBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathExtensionBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathStemBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDot},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathExistsBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDot},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathIsDirBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSIsLinuxBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cLogExt},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathWithExtensionBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cOtherBin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathWithBasenameBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  const auto expected_joined = (std::filesystem::path("/tmp") / "file.txt").string();
  auto expected_with_ext = std::filesystem::path("/tmp/file.txt");
  expected_with_ext.replace_extension(".log");
  auto expected_with_basename = std::filesystem::path("/tmp/file.txt");
  expected_with_basename.replace_filename("other.bin");
#if defined(__linux__)
  constexpr auto kExpectedLinuxFlag = "True";
#else
  constexpr auto kExpectedLinuxFlag = "False";
#endif
  REQUIRE(output.str() == expected_joined + "\nfile.txt\n.txt\nfile\nTrue\nTrue\n" + kExpectedLinuxFlag + "\n" +
                              expected_with_ext.string() + "\n" + expected_with_basename.string() + "\n");
}

TEST_CASE("VM executes Std.String and Std.Path builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc"}});
  const auto cAbc = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp/file.txt"}});
  const auto cPath = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"log"}});
  const auto cLog = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kStringUpperBuiltin = builtin(fleaux::vm::BuiltinId::StringUpper);
  constexpr auto kPathWithExtensionBuiltin = builtin(fleaux::vm::BuiltinId::PathWithExtension);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kStringUpperBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cPath},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cLog},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPathWithExtensionBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  auto expected_with_ext = std::filesystem::path("/tmp/file.txt");
  expected_with_ext.replace_extension(".log");
  REQUIRE(output.str() == "ABC\n" + expected_with_ext.string() + "\n");
}

TEST_CASE("VM executes Std.OS, Std.Tuple, and Std.Dict builtins through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  [[maybe_unused]] constexpr auto kOSIsLinuxBuiltin = builtin(fleaux::vm::BuiltinId::OSIsLinux);
  [[maybe_unused]] constexpr auto kOSIsWindowsBuiltin = builtin(fleaux::vm::BuiltinId::OSIsWindows);
  [[maybe_unused]] constexpr auto kOSIsMacOSBuiltin = builtin(fleaux::vm::BuiltinId::OSIsMacOS);
  constexpr auto kTupleAppendBuiltin = builtin(fleaux::vm::BuiltinId::TupleAppend);
  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictGetBuiltin = builtin(fleaux::vm::BuiltinId::DictGet);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
#ifdef _WIN32
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSIsWindowsBuiltin},
#elif defined(__linux__)
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSIsLinuxBuiltin},
#else
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSIsMacOSBuiltin},
#endif
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleAppendBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n1 2 3\n3\n");
}

TEST_CASE("VM executes Std.OS env and Std.File/Std.Dir builtins through primary builtin dispatch", "[vm]") {
  const auto base = std::filesystem::temp_directory_path() / "fleaux_vm_native_fs_test";
  const auto file_path = (base / "data.txt").string();
  const auto dir_path = base.string();
  const std::string env_key = "FLEAUX_VM_TEST_ENV_KEY";
  const std::string env_value = "vm_native_ok";
  std::filesystem::remove_all(base);

  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{env_key});
  const auto cEnvKey = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{env_value});
  const auto cEnvVal = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{dir_path});
  const auto cDir = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{file_path});
  const auto cFile = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"hello"}});
  const auto cHello = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kOSSetEnvBuiltin = builtin(fleaux::vm::BuiltinId::OSSetEnv);
  constexpr auto kOSEnvBuiltin = builtin(fleaux::vm::BuiltinId::OSEnv);
  constexpr auto kOSUnsetEnvBuiltin = builtin(fleaux::vm::BuiltinId::OSUnsetEnv);
  constexpr auto kDirCreateBuiltin = builtin(fleaux::vm::BuiltinId::DirCreate);
  constexpr auto kFileWriteTextBuiltin = builtin(fleaux::vm::BuiltinId::FileWriteText);
  constexpr auto kFileReadTextBuiltin = builtin(fleaux::vm::BuiltinId::FileReadText);
  constexpr auto kFileDeleteBuiltin = builtin(fleaux::vm::BuiltinId::FileDelete);
  constexpr auto kDirDeleteBuiltin = builtin(fleaux::vm::BuiltinId::DirDelete);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvVal},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSSetEnvBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSEnvBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kOSUnsetEnvBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDir},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDirCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cHello},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFileWriteTextBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFileReadTextBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kFileDeleteBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDir},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDirDeleteBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  std::filesystem::remove_all(base);
#if defined(_WIN32)
  _putenv_s(env_key.c_str(), "");
#else
  unsetenv(env_key.c_str());
#endif

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "vm_native_ok\nvm_native_ok\nTrue\n" + dir_path + "\n" + file_path + "\nhello\nTrue\nTrue\n");
}

TEST_CASE("VM executes Std.Dict.Merge with shared overwrite semantics", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"shared"}});
  const auto cShared = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"base_only"}});
  const auto cBaseOnly = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"overlay_only"}});
  const auto cOverlayOnly = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictMergeBuiltin = builtin(fleaux::vm::BuiltinId::DictMerge);
  constexpr auto kDictGetBuiltin = builtin(fleaux::vm::BuiltinId::DictGet);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cShared},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBaseOnly},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cShared},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cOverlayOnly},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictMergeBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cShared},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBaseOnly},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cOverlayOnly},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "2\n10\n20\n");
}

TEST_CASE("VM builtin catalog resolves callable builtin names and direct runtime functions execute them", "[vm]") {
  const auto require_builtin = [](const std::string_view name) {
    const auto builtin_id = fleaux::vm::builtin_id_from_name(name);
    REQUIRE(builtin_id.has_value());
    REQUIRE(fleaux::vm::builtin_name(*builtin_id) == name);
    return *builtin_id;
  };

  REQUIRE(require_builtin("Std.UnaryPlus") == fleaux::vm::BuiltinId::UnaryPlus);
  REQUIRE(require_builtin("Std.UnaryMinus") == fleaux::vm::BuiltinId::UnaryMinus);
  REQUIRE(require_builtin("Std.Add") == fleaux::vm::BuiltinId::Add);
  REQUIRE(require_builtin("Std.Bit.And") == fleaux::vm::BuiltinId::BitAnd);
  REQUIRE(require_builtin("Std.Bit.ShiftLeft") == fleaux::vm::BuiltinId::BitShiftLeft);
  REQUIRE(require_builtin("Std.GreaterThan") == fleaux::vm::BuiltinId::GreaterThan);
  REQUIRE(require_builtin("Std.Not") == fleaux::vm::BuiltinId::Not);
  REQUIRE(require_builtin("Std.Select") == fleaux::vm::BuiltinId::Select);
  REQUIRE(require_builtin("Std.Match") == fleaux::vm::BuiltinId::Match);
  REQUIRE(require_builtin("Std.Apply") == fleaux::vm::BuiltinId::Apply);
  REQUIRE(require_builtin("Std.Branch") == fleaux::vm::BuiltinId::Branch);
  REQUIRE(require_builtin("Std.Tuple.Append") == fleaux::vm::BuiltinId::TupleAppend);
  REQUIRE(require_builtin("Std.Array.GetAt") == fleaux::vm::BuiltinId::ArrayGetAt);
  REQUIRE(require_builtin("Std.Dict.Merge") == fleaux::vm::BuiltinId::DictMerge);
  REQUIRE(require_builtin("Std.Result.Ok") == fleaux::vm::BuiltinId::ResultOk);
  REQUIRE(require_builtin("Std.Result.Tag") == fleaux::vm::BuiltinId::ResultTag);
  REQUIRE(require_builtin("Std.Try") == fleaux::vm::BuiltinId::Try);
  REQUIRE(require_builtin("Std.Parallel.Map") == fleaux::vm::BuiltinId::ParallelMap);
  REQUIRE(require_builtin("Std.Task.Spawn") == fleaux::vm::BuiltinId::TaskSpawn);
  REQUIRE(require_builtin("Std.Task.Await") == fleaux::vm::BuiltinId::TaskAwait);
  REQUIRE(require_builtin("Std.Wrap") == fleaux::vm::BuiltinId::Wrap);
  REQUIRE(require_builtin("Std.Unwrap") == fleaux::vm::BuiltinId::Unwrap);
  REQUIRE(require_builtin("Std.First") == fleaux::vm::BuiltinId::First);
  REQUIRE(require_builtin("Std.Second") == fleaux::vm::BuiltinId::Second);
  REQUIRE(require_builtin("Std.ElementAt") == fleaux::vm::BuiltinId::ElementAt);
  REQUIRE(require_builtin("Std.Length") == fleaux::vm::BuiltinId::Length);
  REQUIRE(require_builtin("Std.Take") == fleaux::vm::BuiltinId::Take);
  REQUIRE(require_builtin("Std.Drop") == fleaux::vm::BuiltinId::Drop);
  REQUIRE(require_builtin("Std.Slice") == fleaux::vm::BuiltinId::SliceTupleUInt64);
  REQUIRE(require_builtin("Std.Cast") == fleaux::vm::BuiltinId::Cast);
  REQUIRE(require_builtin("Std.ToInt64") == fleaux::vm::BuiltinId::ToInt64);
  REQUIRE(require_builtin("Std.ToUInt64") == fleaux::vm::BuiltinId::ToUInt64);
  REQUIRE(require_builtin("Std.ToFloat64") == fleaux::vm::BuiltinId::ToFloat64);
  REQUIRE(require_builtin("Std.Random.Create") == fleaux::vm::BuiltinId::RandomCreate);
  REQUIRE(require_builtin("Std.Random.NextUInt64") == fleaux::vm::BuiltinId::RandomNextUInt64);
  REQUIRE(require_builtin("Std.Random.NextInt64") == fleaux::vm::BuiltinId::RandomNextInt64);
  REQUIRE(require_builtin("Std.Random.NextFloat64") == fleaux::vm::BuiltinId::RandomNextFloat64);
  REQUIRE(require_builtin("Std.Random.NextBool") == fleaux::vm::BuiltinId::RandomNextBool);
  REQUIRE(require_builtin("Std.Random.Split") == fleaux::vm::BuiltinId::RandomSplit);
  REQUIRE(require_builtin("Std.Math.Sqrt") == fleaux::vm::BuiltinId::Sqrt);
  REQUIRE(require_builtin("Std.Math.Sin") == fleaux::vm::BuiltinId::Sin);
  REQUIRE(require_builtin("Std.Math.Cos") == fleaux::vm::BuiltinId::Cos);
  REQUIRE(require_builtin("Std.Math.Tan") == fleaux::vm::BuiltinId::Tan);
  REQUIRE(require_builtin("Std.Math.Floor") == fleaux::vm::BuiltinId::MathFloor);
  REQUIRE(require_builtin("Std.Math.Ceil") == fleaux::vm::BuiltinId::MathCeil);
  REQUIRE(require_builtin("Std.Math.Abs") == fleaux::vm::BuiltinId::MathAbs);
  REQUIRE(require_builtin("Std.Math.Log") == fleaux::vm::BuiltinId::MathLog);
  REQUIRE(require_builtin("Std.Math.Clamp") == fleaux::vm::BuiltinId::MathClamp);
  REQUIRE(require_builtin("Std.ToString") == fleaux::vm::BuiltinId::ToString);
  REQUIRE(require_builtin("Std.ToNum") == fleaux::vm::BuiltinId::ToNum);
  REQUIRE(require_builtin("Std.String.ParseInt64") == fleaux::vm::BuiltinId::StringParseInt64);
  REQUIRE(require_builtin("Std.String.ParseUInt64") == fleaux::vm::BuiltinId::StringParseUInt64);
  REQUIRE(require_builtin("Std.String.ParseFloat64") == fleaux::vm::BuiltinId::StringParseFloat64);
  REQUIRE(require_builtin("Std.String.Upper") == fleaux::vm::BuiltinId::StringUpper);
  REQUIRE(require_builtin("Std.String.Regex.Find") == fleaux::vm::BuiltinId::StringRegexFind);

  REQUIRE(fleaux::runtime::to_double(
              fleaux::runtime::UnaryPlus(fleaux::runtime::make_tuple(fleaux::runtime::make_int(7)))) == 7.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::UnaryMinus(fleaux::runtime::make_int(7))) == -7.0);

  const auto sum =
      fleaux::runtime::Add(fleaux::runtime::make_tuple(fleaux::runtime::make_uint(40), fleaux::runtime::make_uint(2)));
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Type(sum)) == "UInt64");
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::BitAnd(
              fleaux::runtime::make_tuple(fleaux::runtime::make_int(6), fleaux::runtime::make_int(3)))) == 2.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::BitShiftLeft(
              fleaux::runtime::make_tuple(fleaux::runtime::make_int(3), fleaux::runtime::make_int(2)))) == 12.0);
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::GreaterThan(
      fleaux::runtime::make_tuple(fleaux::runtime::make_uint(2), fleaux::runtime::make_int(-1)))));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::Not(fleaux::runtime::make_bool(false))));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Select(fleaux::runtime::make_tuple(
              fleaux::runtime::make_bool(false), fleaux::runtime::make_int(10), fleaux::runtime::make_int(20)))) ==
          20.0);

  const auto add_one =
      fleaux::runtime::make_callable_ref([](const fleaux::runtime::Value& value) -> fleaux::runtime::Value {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(value) + 1);
      });
  const auto sub_one =
      fleaux::runtime::make_callable_ref([](const fleaux::runtime::Value& value) -> fleaux::runtime::Value {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(value) - 1);
      });
  const auto even_handler = fleaux::runtime::make_callable_ref(
      [](const fleaux::runtime::Value&) -> fleaux::runtime::Value { return fleaux::runtime::make_string("even"); });
  const auto odd_handler = fleaux::runtime::make_callable_ref(
      [](const fleaux::runtime::Value&) -> fleaux::runtime::Value { return fleaux::runtime::make_string("odd"); });
  const auto is_even =
      fleaux::runtime::make_callable_ref([](const fleaux::runtime::Value& value) -> fleaux::runtime::Value {
        return fleaux::runtime::make_bool(fleaux::runtime::as_int_value(value) % 2 == 0);
      });

  REQUIRE(fleaux::runtime::to_double(
              fleaux::runtime::Apply(fleaux::runtime::make_tuple(fleaux::runtime::make_int(41), add_one))) == 42.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Branch(fleaux::runtime::make_tuple(
              fleaux::runtime::make_bool(true), fleaux::runtime::make_int(10), add_one, sub_one))) == 11.0);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Match(fleaux::runtime::make_tuple(
              fleaux::runtime::make_int(6), fleaux::runtime::make_tuple(is_even, even_handler),
              fleaux::runtime::make_tuple(fleaux::runtime::make_string("__fleaux_match_wildcard__"), odd_handler)))) ==
          "even");

  const auto appended = fleaux::runtime::TupleAppend(fleaux::runtime::make_tuple(
      fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2)),
      fleaux::runtime::make_int(3)));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(appended, 2)) == 3.0);

  const auto array_value = fleaux::runtime::ArrayGetAt(fleaux::runtime::make_tuple(
      fleaux::runtime::make_tuple(fleaux::runtime::make_int(10), fleaux::runtime::make_int(20)),
      fleaux::runtime::make_int(1)));
  REQUIRE(fleaux::runtime::to_double(array_value) == 20.0);

  fleaux::runtime::Value base{fleaux::runtime::Object{}};
  fleaux::runtime::as_object(base)[fleaux::runtime::dict_key_from_validated_value(fleaux::runtime::make_string("shared"), "test")] =
      fleaux::runtime::make_int(1);
  fleaux::runtime::Value overlay{fleaux::runtime::Object{}};
  fleaux::runtime::as_object(overlay)[fleaux::runtime::dict_key_from_validated_value(fleaux::runtime::make_string("shared"), "test")] =
      fleaux::runtime::make_int(2);

  const auto merged = fleaux::runtime::DictMerge(fleaux::runtime::make_tuple(base, overlay));
  REQUIRE(fleaux::runtime::to_double(*fleaux::runtime::as_object(merged).TryGet("s:shared")) == 2.0);

  const auto ok = fleaux::runtime::ResultOk(fleaux::runtime::make_int(5));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::ResultTag(ok)));

  const auto try_result = fleaux::runtime::Try(fleaux::runtime::make_tuple(fleaux::runtime::make_int(41), add_one));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::ResultIsOk(try_result)));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::ResultUnwrap(try_result)) == 42.0);

  REQUIRE(fleaux::runtime::as_int_value(fleaux::runtime::StringParseInt64(
              fleaux::runtime::make_tuple(fleaux::runtime::make_string("-42")))) == -42);
  REQUIRE(fleaux::runtime::is_uint_number(
      fleaux::runtime::StringParseUInt64(fleaux::runtime::make_tuple(fleaux::runtime::make_string("42")))));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::StringParseFloat64(
              fleaux::runtime::make_tuple(fleaux::runtime::make_string("6.25e1")))) == 62.5);

  const auto mapped = fleaux::runtime::ParallelMap(fleaux::runtime::make_tuple(
      fleaux::runtime::make_tuple(fleaux::runtime::make_int(1), fleaux::runtime::make_int(2),
                                  fleaux::runtime::make_int(3)),
      add_one));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::ResultIsOk(mapped)));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(fleaux::runtime::ResultUnwrap(mapped), 2)) == 4.0);

  fleaux::runtime::TaskRegistryScope task_scope;
  const auto task = fleaux::runtime::TaskSpawn(fleaux::runtime::make_tuple(add_one, fleaux::runtime::make_int(41)));
  const auto awaited = fleaux::runtime::TaskAwait(fleaux::runtime::make_tuple(task));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::ResultIsOk(awaited)));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::ResultUnwrap(awaited)) == 42.0);

  const auto wrapped = fleaux::runtime::Wrap(fleaux::runtime::make_int(7));
  REQUIRE(fleaux::runtime::as_array(wrapped).Size() == 1);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Unwrap(wrapped)) == 7.0);

  const auto pair = fleaux::runtime::make_tuple(fleaux::runtime::make_int(7), fleaux::runtime::make_string("rng"));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::First(pair)) == 7.0);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Second(pair)) == "rng");

  const auto seq = fleaux::runtime::make_tuple(fleaux::runtime::make_int(10), fleaux::runtime::make_int(20),
                                               fleaux::runtime::make_int(30));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Length(seq)) == 3.0);
  REQUIRE(fleaux::runtime::to_double(
              fleaux::runtime::ElementAt(fleaux::runtime::make_tuple(seq, fleaux::runtime::make_int(1)))) == 20.0);
  REQUIRE(
      fleaux::runtime::as_array(fleaux::runtime::Take(fleaux::runtime::make_tuple(seq, fleaux::runtime::make_int(2))))
          .Size() == 2);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(
              fleaux::runtime::Drop(fleaux::runtime::make_tuple(seq, fleaux::runtime::make_int(1))), 0)) == 20.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::array_at(
              fleaux::runtime::SliceTupleUInt64UInt64UInt64(fleaux::runtime::make_tuple(
                  seq, fleaux::runtime::make_int(0), fleaux::runtime::make_int(3), fleaux::runtime::make_int(2))),
              1)) == 30.0);

  const auto as_int = fleaux::runtime::ToInt64(fleaux::runtime::make_float(3.0));
  REQUIRE(fleaux::runtime::to_double(as_int) == 3.0);

  const auto as_uint = fleaux::runtime::ToUInt64(fleaux::runtime::make_int(42));
  REQUIRE(fleaux::runtime::is_uint_number(as_uint));
  REQUIRE(fleaux::runtime::to_double(as_uint) == 42.0);

  const auto as_float = fleaux::runtime::ToFloat64(fleaux::runtime::make_int(7));
  REQUIRE(fleaux::runtime::is_float_number(as_float));
  REQUIRE(fleaux::runtime::to_double(as_float) == 7.0);

  const auto generator =
      fleaux::runtime::RandomCreate(fleaux::runtime::make_tuple(fleaux::runtime::make_uint(123456789ULL)));
  const auto random_uint = fleaux::runtime::RandomNextUInt64(generator);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Type(fleaux::runtime::First(random_uint))) == "UInt64");
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(random_uint))) ==
          "2466975172287755897");

  const auto random_int = fleaux::runtime::RandomNextInt64(
      fleaux::runtime::RandomCreate(fleaux::runtime::make_tuple(fleaux::runtime::make_uint(0ULL))));
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Type(fleaux::runtime::First(random_int))) == "Int64");
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(random_int))) ==
          "-2152535657050944081");

  const auto random_float = fleaux::runtime::RandomNextFloat64(fleaux::runtime::Second(random_uint));
  REQUIRE(fleaux::runtime::is_float_number(fleaux::runtime::First(random_float)));
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::First(random_float)) >= 0.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::First(random_float)) < 1.0);

  const auto random_bool = fleaux::runtime::RandomNextBool(generator);
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::First(random_bool)));

  const auto split = fleaux::runtime::RandomSplit(generator);
  const auto left_step = fleaux::runtime::RandomNextUInt64(fleaux::runtime::First(split));
  const auto right_step = fleaux::runtime::RandomNextUInt64(fleaux::runtime::Second(split));
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(left_step))) ==
          "1278849761381179246");
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::ToString(fleaux::runtime::First(right_step))) ==
          "15883818742529415426");

  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::Sqrt(fleaux::runtime::make_tuple(fleaux::runtime::make_int(9)))) -
                   3.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::Sin(fleaux::runtime::make_tuple(fleaux::runtime::make_int(0)))) -
                   0.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::Cos(fleaux::runtime::make_tuple(fleaux::runtime::make_int(0)))) -
                   1.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::Tan(fleaux::runtime::make_tuple(fleaux::runtime::make_int(0)))) -
                   0.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::MathFloor(fleaux::runtime::make_tuple(fleaux::runtime::make_float(2.9)))) -
                   2.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::MathCeil(fleaux::runtime::make_tuple(fleaux::runtime::make_float(2.1)))) -
                   3.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::MathAbs(fleaux::runtime::make_tuple(fleaux::runtime::make_float(-7.5)))) -
                   7.5) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(
                       fleaux::runtime::MathLog(fleaux::runtime::make_tuple(fleaux::runtime::make_int(1)))) -
                   0.0) < 1e-12);
  REQUIRE(std::abs(fleaux::runtime::to_double(fleaux::runtime::MathClamp(fleaux::runtime::make_tuple(
                       fleaux::runtime::make_int(9), fleaux::runtime::make_int(0), fleaux::runtime::make_int(5)))) -
                   5.0) < 1e-12);

  const auto stringified = fleaux::runtime::ToString(fleaux::runtime::make_int(42));
  REQUIRE(fleaux::runtime::as_string(stringified) == "42");

  const auto numeric = fleaux::runtime::ToNum(fleaux::runtime::make_tuple(fleaux::runtime::make_string("42.5")));
  REQUIRE(fleaux::runtime::to_double(numeric) == 42.5);

  const auto uppercased = fleaux::runtime::StringUpper(fleaux::runtime::make_string("hElLo"));
  REQUIRE(fleaux::runtime::as_string(uppercased) == "HELLO");

  const auto regex_match_offset = fleaux::runtime::StringRegexFind(
      fleaux::runtime::make_tuple(fleaux::runtime::make_string("abc-123 xyz"), fleaux::runtime::make_string("[0-9]+")));
  REQUIRE(fleaux::runtime::to_double(regex_match_offset) == 4.0);
}

TEST_CASE(
    "VM builtin catalog resolves loop, io, help, and exit builtin names and direct runtime functions execute them",
    "[vm]") {
  const auto require_builtin = [](const std::string_view name) {
    const auto builtin_id = fleaux::vm::builtin_id_from_name(name);
    REQUIRE(builtin_id.has_value());
    REQUIRE(fleaux::vm::builtin_name(*builtin_id) == name);
    return *builtin_id;
  };

  REQUIRE(require_builtin("Std.Loop") == fleaux::vm::BuiltinId::Loop);
  REQUIRE(require_builtin("Std.LoopN") == fleaux::vm::BuiltinId::LoopN);
  REQUIRE(require_builtin("Std.Printf") == fleaux::vm::BuiltinId::Printf);
  REQUIRE(require_builtin("Std.Println") == fleaux::vm::BuiltinId::Println);
  REQUIRE(require_builtin("Std.GetArgs") == fleaux::vm::BuiltinId::GetArgs);
  REQUIRE(require_builtin("Std.Type") == fleaux::vm::BuiltinId::Type);
  REQUIRE(require_builtin("Std.Input") == fleaux::vm::BuiltinId::InputVoid);
  REQUIRE(require_builtin("Std.Help") == fleaux::vm::BuiltinId::Help);
  REQUIRE(require_builtin("Std.Exit") == fleaux::vm::BuiltinId::ExitVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Input#0") == fleaux::vm::BuiltinId::InputVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Input#1") == fleaux::vm::BuiltinId::InputString);

  std::ostringstream output;
  fleaux::runtime::RuntimeOutputStreamScope output_scope(output);
  std::istringstream input{"Ada\n"};
  fleaux::runtime::RuntimeInputStreamScope input_scope(input);

  auto set_args = [](const std::vector<std::string>& args) {
    std::vector<std::string> args_storage = args;
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) {
      argv_ptrs.push_back(arg.data());
    }
    fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  };

  set_args({"fleaux", "sample.fleaux", "--flag"});
  fleaux::runtime::clear_help_metadata_registry();
  fleaux::runtime::register_help_metadata(fleaux::runtime::HelpMetadata{
      .name = "Std.Add",
      .signature = "let Std.Add(lhs: Int64, rhs: Int64): Int64 :: __builtin__",
      .doc_lines = {"@brief Adds two integers."},
      .is_builtin = true,
  });

  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Println(fleaux::runtime::make_int(8))) == 8.0);

  const auto printf_result = fleaux::runtime::Printf(fleaux::runtime::make_tuple(
      fleaux::runtime::make_string("{} + {}"), fleaux::runtime::make_int(1), fleaux::runtime::make_int(2)));
  REQUIRE(fleaux::runtime::as_array(printf_result).Size() == 3U);
  const auto printf_literal = fleaux::runtime::Printf(fleaux::runtime::make_string("hello"));
  REQUIRE(fleaux::runtime::as_array(printf_literal).Size() == 1U);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::Type(fleaux::runtime::make_uint(7))) == "UInt64");

  const auto args = fleaux::runtime::GetArgs(fleaux::runtime::make_tuple());
  REQUIRE(fleaux::runtime::as_array(args).Size() == 3U);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::array_at(args, 2)) == "--flag");

  const auto help = fleaux::runtime::Help(fleaux::runtime::make_string("Std.Add"));
  REQUIRE_THAT(fleaux::runtime::as_string(help), Catch::Matchers::ContainsSubstring("Help on function Std.Add"));

  const auto input_value = fleaux::runtime::Input_String(fleaux::runtime::make_string("name> "));
  REQUIRE(fleaux::runtime::as_string(input_value) == "Ada");

  const auto continue_func =
      fleaux::runtime::make_callable_ref([](const fleaux::runtime::Value& value) -> fleaux::runtime::Value {
        return fleaux::runtime::make_bool(fleaux::runtime::as_int_value(value) > 0);
      });
  const auto step_func =
      fleaux::runtime::make_callable_ref([](const fleaux::runtime::Value& value) -> fleaux::runtime::Value {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(value) - 1);
      });
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::Loop(
              fleaux::runtime::make_tuple(fleaux::runtime::make_int(3), continue_func, step_func))) == 0.0);
  REQUIRE(fleaux::runtime::to_double(fleaux::runtime::LoopN(fleaux::runtime::make_tuple(
              fleaux::runtime::make_int(3), continue_func, step_func, fleaux::runtime::make_int(3)))) == 0.0);

  REQUIRE(output.str() == "8\n1 + 2helloname> ");
  fleaux::runtime::clear_help_metadata_registry();
}

TEST_CASE("VM builtin catalog resolves OS/path/file/dir builtin names and direct runtime functions execute them",
          "[vm]") {
  const auto require_builtin = [](const std::string_view name) {
    const auto builtin_id = fleaux::vm::builtin_id_from_name(name);
    REQUIRE(builtin_id.has_value());
    REQUIRE(fleaux::vm::builtin_name(*builtin_id) == name);
    return *builtin_id;
  };

  REQUIRE(require_builtin("Std.OS.SetEnv") == fleaux::vm::BuiltinId::OSSetEnv);
  REQUIRE(require_builtin("Std.OS.Env") == fleaux::vm::BuiltinId::OSEnv);
  REQUIRE(require_builtin("Std.OS.UnsetEnv") == fleaux::vm::BuiltinId::OSUnsetEnv);
  REQUIRE(require_builtin("Std.Path.Join") == fleaux::vm::BuiltinId::PathJoin);
  REQUIRE(require_builtin("Std.Path.WithExtension") == fleaux::vm::BuiltinId::PathWithExtension);
  REQUIRE(require_builtin("Std.Dir.Create") == fleaux::vm::BuiltinId::DirCreate);
  REQUIRE(require_builtin("Std.Dir.Delete") == fleaux::vm::BuiltinId::DirDelete);
  REQUIRE(require_builtin("Std.File.WriteText") == fleaux::vm::BuiltinId::FileWriteText);
  REQUIRE(require_builtin("Std.File.ReadText") == fleaux::vm::BuiltinId::FileReadText);
  REQUIRE(require_builtin("Std.File.Delete") == fleaux::vm::BuiltinId::FileDelete);

  const auto base = std::filesystem::temp_directory_path() / "fleaux_vm_builtin_dispatch_fs_test";
  const auto file_path = (base / "data.txt").string();
  const auto dir_path = base.string();
  const std::string env_key = "FLEAUX_VM_FALLBACK_ENV_KEY";
  const std::string env_value = "vm_builtin_dispatch_ok";
  std::filesystem::remove_all(base);

  const auto joined = fleaux::runtime::PathJoin(
      fleaux::runtime::make_tuple(fleaux::runtime::make_string("/tmp"), fleaux::runtime::make_string("file.txt")));
  REQUIRE(fleaux::runtime::as_string(joined) == (std::filesystem::path("/tmp") / "file.txt").string());
  const auto joined_nested = fleaux::runtime::PathJoin(
      fleaux::runtime::make_tuple(fleaux::runtime::make_string("/tmp"), fleaux::runtime::make_string("nested"),
                                  fleaux::runtime::make_string("file.txt")));
  REQUIRE(fleaux::runtime::as_string(joined_nested) ==
          (std::filesystem::path("/tmp") / "nested" / "file.txt").string());
  const auto with_ext = fleaux::runtime::PathWithExtension(
      fleaux::runtime::make_tuple(fleaux::runtime::make_string("/tmp/file.txt"), fleaux::runtime::make_string("log")));
  auto expected_with_ext = std::filesystem::path("/tmp/file.txt");
  expected_with_ext.replace_extension(".log");
  REQUIRE(fleaux::runtime::as_string(with_ext) == expected_with_ext.string());

  const auto created_dir = fleaux::runtime::DirCreate(fleaux::runtime::make_string(dir_path));
  REQUIRE(fleaux::runtime::as_string(created_dir) == dir_path);
  REQUIRE(std::filesystem::exists(base));

  const auto written_path = fleaux::runtime::FileWriteText(
      fleaux::runtime::make_tuple(fleaux::runtime::make_string(file_path), fleaux::runtime::make_string("hello")));
  REQUIRE(fleaux::runtime::as_string(written_path) == file_path);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::FileReadText(fleaux::runtime::make_string(file_path))) ==
          "hello");
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::FileDelete(fleaux::runtime::make_string(file_path))));
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::DirDelete(fleaux::runtime::make_string(dir_path))));

  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::OSSetEnv(fleaux::runtime::make_tuple(
              fleaux::runtime::make_string(env_key), fleaux::runtime::make_string(env_value)))) == env_value);
  REQUIRE(fleaux::runtime::as_string(fleaux::runtime::OSEnv(fleaux::runtime::make_string(env_key))) == env_value);
  REQUIRE(fleaux::runtime::as_bool(fleaux::runtime::OSUnsetEnv(fleaux::runtime::make_string(env_key))));

#if defined(_WIN32)
  _putenv_s(env_key.c_str(), "");
#else
  unsetenv(env_key.c_str());
#endif
}

TEST_CASE("VM kCallBuiltin executes Std.Tuple and Std.Dict helpers through primary builtin dispatch", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c0 = push_i64_const(bytecode_module, 0);
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c9 = push_i64_const(bytecode_module, 9);
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"missing"}});
  const auto cMissing = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  constexpr auto kTupleAppendBuiltin = builtin(fleaux::vm::BuiltinId::TupleAppend);
  constexpr auto kTuplePrependBuiltin = builtin(fleaux::vm::BuiltinId::TuplePrepend);
  constexpr auto kTupleContainsBuiltin = builtin(fleaux::vm::BuiltinId::TupleContains);
  constexpr auto kTupleZipBuiltin = builtin(fleaux::vm::BuiltinId::TupleZip);
  constexpr auto kDictCreateBuiltin = builtin(fleaux::vm::BuiltinId::DictCreateVoid);
  constexpr auto kDictSetBuiltin = builtin(fleaux::vm::BuiltinId::DictSet);
  constexpr auto kDictGetBuiltin = builtin(fleaux::vm::BuiltinId::DictGet);
  constexpr auto kDictGetDefaultBuiltin = builtin(fleaux::vm::BuiltinId::DictGetDefault);
  constexpr auto kDictLengthBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictLength);
  constexpr auto kDictKeysBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictKeys);
  constexpr auto kDictValuesBuiltin = builtin_direct(fleaux::vm::BuiltinId::DictValues);
  constexpr auto kPrintBuiltin = builtin(fleaux::vm::BuiltinId::Println);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleAppendBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTuplePrependBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleContainsBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kTupleZipBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictCreateBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictSetBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictLengthBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictKeysBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictValuesBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMissing},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kDictGetDefaultBuiltin},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n0 1 2\nTrue\n(1, 3) (2, 4)\n9\n1\nk\n9\n42\n");
}

TEST_CASE("RuntimeSession parameter lists pass items as arguments and tuple values as one argument",
          "[vm][repl][parameter-lists]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Pair(a: Int64, b: Int64): Int64 = (a, b) -> +;\n"
      "let Whole(t: Tuple<Int64, Int64>): Int64 = t -> Std.Length;\n"
      "let V(xs: Int64...): Int64 = xs -> Std.Length;\n"
      "let P: Tuple<Int64, Int64> = (1, 2);\n"
      "let Many: Tuple<Int64...> = (1, 2, 3);\n"
      "let T: Tuple<Int64> = (7,);\n"
      "(1, 2) -> Pair -> Std.Println;\n"
      "(P...) -> Pair -> Std.Println;\n"
      "P -> Whole -> Std.Println;\n"
      "(1, 2,) -> Whole -> Std.Println;\n"
      "((1, 2)) -> Whole -> Std.Println;\n"
      "5 -> V -> Std.Println;\n"
      "(Many...) -> V -> Std.Println;\n"
      "(9, Many...) -> V -> Std.Println;\n"
      "(P..., Many...) -> V -> Std.Println;\n"
      "(5,) -> Std.Println;\n"
      "T -> Std.Type -> Std.Println;\n"
      "((1, 2,), Whole) -> Std.Apply -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n3\n2\n2\n2\n1\n3\n4\n5\n(5,)\nTuple\n2\n");
}

TEST_CASE("RuntimeSession calls function values named as call targets", "[vm][repl][function-values]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> +;\n"
      "let Add2(a: Int64, b: Int64): Int64 = (a, b) -> +;\n"
      "let Count(xs: Int64...): Int64 = xs -> Std.Length;\n"
      "let Whole(t: Tuple<Int64, Int64>): Int64 = t -> Std.Length;\n"
      "let P: Tuple<Int64, Int64> = (3, 4);\n"
      "let OnFive(f: (Int64) => Int64): Int64 = 5 -> f;\n"
      "let OnPair(f: (Int64, Int64) => Int64): Int64 = (3, 4) -> f;\n"
      "let OnSpread(f: (Int64, Int64) => Int64): Int64 = (P...) -> f;\n"
      "let OnTuple(f: (Tuple<Int64, Int64>) => Int64): Int64 = (1, 2,) -> f;\n"
      "let OnMany(f: (Int64...) => Int64): Tuple<Int64, Int64, Int64> = (5 -> f, (1, 2, 3) -> f, () -> f);\n"
      "let InBlock(x: Int64): Int64 = { let f: (Int64) => Int64 = Inc; x -> f -> f; };\n"
      "let InClosure(f: (Int64) => Int64): Int64 = 5 -> (x: Int64): Int64 = x -> f;\n"
      "let Twice: (Int64, Int64) => Int64 = Add2;\n"
      "Inc -> OnFive -> Std.Println;\n"
      "Add2 -> OnPair -> Std.Println;\n"
      "Std.Add -> OnPair -> Std.Println;\n"
      "Add2 -> OnSpread -> Std.Println;\n"
      "Whole -> OnTuple -> Std.Println;\n"
      "Count -> OnMany -> Std.Println;\n"
      "5 -> InBlock -> Std.Println;\n"
      "Inc -> InClosure -> Std.Println;\n"
      "(10, 20) -> Twice -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "6\n7\n7\n7\n2\n(1, 3, 0)\n7\n6\n30\n");
}

TEST_CASE("RuntimeSession closure stages take their arguments by the parameter-list rule",
          "[vm][repl][function-values][closures]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let P: Tuple<Int64, Int64> = (3, 4);\n"
      "((1, 2) -> (a: Int64, b: Int64): Int64 = (a, b) -> +) -> Std.Println;\n"
      "((P...) -> (a: Int64, b: Int64): Int64 = (a, b) -> *) -> Std.Println;\n"
      "(5 -> (x: Int64): Int64 = (x, 1) -> +) -> Std.Println;\n"
      "((5) -> (x: Int64): Int64 = (x, 1) -> +) -> Std.Println;\n"
      "(P -> (t: Tuple<Int64, Int64>): Int64 = t -> Std.Length) -> Std.Println;\n"
      "((1, 2,) -> (t: Tuple<Int64, Int64>): Int64 = t -> Std.Length) -> Std.Println;\n"
      "(() -> (): Int64 = 7) -> Std.Println;\n"
      "((1, 2, 3) -> (xs: Int64...): Int64 = xs -> Std.Length) -> Std.Println;\n"
      "(4 -> (xs: Int64...): Int64 = xs -> Std.Length) -> Std.Println;\n"
      "(5 -> <T>(x: T): Tuple<T, T> = (x, x)) -> Std.Println;\n"
      "((1, \"a\") -> <A, B>(a: A, b: B): Tuple<B, A> = (b, a)) -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n12\n6\n6\n2\n2\n7\n3\n1\n(5, 5)\n(a, 1)\n");
}

TEST_CASE("RuntimeSession closures with block bodies capture outer values", "[vm][repl][closures]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> +;\n"
      "let CallsCaptured(f: (Int64) => Int64): Int64 = 5 -> (x: Int64): Int64 = { let y: Int64 = x -> f; (y, 1) -> +; };\n"
      "let ShadowedInBlock(n: Int64): Int64 = 1 -> (x: Int64): Int64 = { let n: Int64 = 10; (x, n) -> +; };\n"
      "let OuterBeforeShadow(n: Int64): Int64 = 1 -> (x: Int64): Int64 = { let m: Int64 = (n, 1) -> +; let n: Int64 = 100; (m, n) -> +; };\n"
      "Inc -> CallsCaptured -> Std.Println;\n"
      "3 -> ShadowedInBlock -> Std.Println;\n"
      "3 -> OuterBeforeShadow -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "7\n11\n104\n");
}

TEST_CASE("RuntimeSession generic variadic closure stages bind their type arguments", "[vm][repl][closures]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Many: Tuple<Int64...> = (1, 2, 3);\n"
      "let Twice<T>(x: T): Tuple<T, T> = x -> <T>(y: T): Tuple<T, T> = (y, y);\n"
      "((1, 2, 3) -> <T>(xs: T...): Int64 = xs -> Std.Length) -> Std.Println;\n"
      "((Many...) -> <T>(xs: T...): Int64 = xs -> Std.Length) -> Std.Println;\n"
      "(4 -> Twice) -> Std.Println;\n"
      "let Pair: Tuple<Int64, Int64> = (1, 2);\n"
      "(Pair -> <T>(xs: T...): Int64 = xs -> Std.Length) -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n3\n(4, 4)\n1\n");
}

TEST_CASE("RuntimeSession tuple templates can be followed by closures", "[vm][repl][closures]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let P: Tuple<Int64, Int64> = (4, 5);\n"
      "((5, 6) -> Std.Add -> (_, 1) -> (a: Int64, b: Int64): Int64 = (a, b) -> -) -> Std.Println;\n"
      "(7 -> (_, 2, _) -> (a: Int64, b: Int64, c: Int64): Int64 = ((a, b) -> +, c) -> *) -> Std.Println;\n"
      "(3 -> (_,) -> (t: Tuple<Int64>): Int64 = t -> Std.Length) -> Std.Println;\n"
      "(P -> (_..., 1) -> (a: Int64, b: Int64, c: Int64): Int64 = ((a, b) -> +, c) -> +) -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n63\n1\n10\n");
}

TEST_CASE("RuntimeSession spreads reach overloads and function values in generic bodies",
          "[vm][repl][parameter-lists][function-values]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto result = session.run_snippet(
      "import Std;\n"
      "let Combine(a: Int64, b: Int64): Int64 = (a, b) -> +;\n"
      "let Combine(a: String, b: String): String = (\"{}{}\", a, b) -> Std.String.Format;\n"
      "let Ints: Tuple<Int64, Int64> = (2, 3);\n"
      "let Strs: Tuple<String, String> = (\"a\", \"b\");\n"
      "let ApplyTo<T, U>(f: (T) => U, x: T): U = x -> f;\n"
      "let Fold<T>(f: (T, T) => T, pair: Tuple<T, T>): T = (pair...) -> f;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> +;\n"
      "(Ints...) -> Combine -> Std.Println;\n"
      "(Strs...) -> Combine -> Std.Println;\n"
      "(Inc, 4) -> ApplyTo -> Std.Println;\n"
      "(Std.Multiply, (6, 7)) -> Fold -> Std.Println;\n",
      output);

  if (!result.has_value()) {
    INFO(result.error().message);
  }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "5\nab\n5\n42\n");
}
