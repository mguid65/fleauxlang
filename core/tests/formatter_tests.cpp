#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/format/formatter.hpp"
#include "fleaux/frontend/parser.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for formatter tests."
#endif

namespace {

using fleaux::frontend::format::format_source;
using fleaux::frontend::format::FormatOptions;

// Formats the source and checks that formatting the result again changes nothing.
auto format(const std::string& source, const std::size_t width = 100) -> std::string {
  const FormatOptions options{.width = width, .indent = 2};
  const auto formatted = format_source(source, "formatter_test.fleaux", options);
  INFO((formatted.has_value() ? std::string{} : formatted.error().message));
  REQUIRE(formatted.has_value());
  const auto again = format_source(*formatted, "formatter_test.fleaux", options);
  REQUIRE(again.has_value());
  CHECK(*again == *formatted);
  return *formatted;
}

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

TEST_CASE("Formatter normalizes spacing", "[formatter]") {
  CHECK(format("import   Std ;\nlet  Inc( x :Float64 ):Float64=(x,1)->Std.Add ;\n") ==
        "import Std;\n"
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n");
}

TEST_CASE("Formatter keeps literals as written", "[formatter]") {
  CHECK(format(R"((1.50, 42u64, -3, "a\tb\"c", True, null) -> Std.Println;)") ==
        "(1.50, 42u64, -3, \"a\\tb\\\"c\", True, null) -> Std.Println;\n");
  CHECK(format("(- 3) -> Std.Println;") == "(-3) -> Std.Println;\n");
}

TEST_CASE("Formatter keeps every token as written", "[formatter]") {
  CHECK(format("type Username :: String;\ntype Id = Int64;\n") == "type Username :: String;\ntype Id = Int64;\n");
  CHECK(format("import 20_export;") == "import 20_export;\n");
  CHECK(format("let X: Tuple<Int64>= (1,);") == "let X: Tuple<Int64> = (1,);\n");
  CHECK(format("alias Num = Int64|Float64;") == "alias Num = Int64 | Float64;\n");
  CHECK(format("let F(f: (Int64)= >Int64, xs: Tuple<Int64...>): Dict<String,Any> = (f, xs) -> Std.Apply;") ==
        "let F(f: (Int64) => Int64, xs: Tuple<Int64...>): Dict<String, Any> = (f, xs) -> Std.Apply;\n");
  CHECK(format("let Id<T: Numeric>(x: T): T = x;") == "let Id<T: Numeric>(x: T): T = x;\n");
}

TEST_CASE("Formatter breaks a long pipeline one call per line", "[formatter]") {
  CHECK(format("(1, 2) -> Std.Add -> (_, 3) -> Std.Multiply -> Std.Println;", 40) ==
        "(1, 2)\n"
        "  -> Std.Add\n"
        "  -> (_, 3) -> Std.Multiply\n"
        "  -> Std.Println;\n");
}

TEST_CASE("Formatter moves a let body that does not fit to the next line", "[formatter]") {
  CHECK(format("let Scale(x: Float64): Float64 = (x, 2.0) -> Std.Multiply;", 40) ==
        "let Scale(x: Float64): Float64 =\n"
        "  (x, 2.0) -> Std.Multiply;\n");
}

TEST_CASE("Formatter keeps a short body on the line of broken parameters", "[formatter]") {
  CHECK(format("let F(alpha: Int64, beta: Int64, gamma: Int64): Int64 = alpha;", 40) ==
        "let F(\n"
        "  alpha: Int64,\n"
        "  beta: Int64,\n"
        "  gamma: Int64\n"
        "): Int64 = alpha;\n");
}

TEST_CASE("Formatter breaks a long tuple one item per line", "[formatter]") {
  CHECK(format(R"(("alpha", "beta", "gamma", "delta") -> Std.Println;)", 30) ==
        "(\n"
        "  \"alpha\",\n"
        "  \"beta\",\n"
        "  \"gamma\",\n"
        "  \"delta\"\n"
        ") -> Std.Println;\n");
}

TEST_CASE("Formatter keeps trailing commas and spreads", "[formatter]") {
  CHECK(format("( x , ) -> Std.Println;") == "(x,) -> Std.Println;\n");
  CHECK(format("(xs ... , 1) -> Std.Println;") == "(xs..., 1) -> Std.Println;\n");
}

TEST_CASE("Formatter keeps comments and lines up trailing ones", "[formatter]") {
  CHECK(format("// Leading comment.\n"
               "import Std;\n"
               "(1) -> Std.Println; // one\n"
               "(22) -> Std.Println;      // twenty-two\n"
               "// Comment at the end.\n") ==
        "// Leading comment.\n"
        "import Std;\n"
        "(1) -> Std.Println;   // one\n"
        "(22) -> Std.Println;  // twenty-two\n"
        "// Comment at the end.\n");
}

TEST_CASE("Formatter breaks a group that holds a trailing comment", "[formatter]") {
  CHECK(format("(1, // first\n 2) -> Std.Println;") ==
        "(\n"
        "  1,  // first\n"
        "  2\n"
        ") -> Std.Println;\n");
}

TEST_CASE("Formatter keeps one blank line between statements", "[formatter]") {
  CHECK(format("\n\nimport Std;\n\n\n\n(1) -> Std.Println;\n(2) -> Std.Println;\n\n") ==
        "import Std;\n"
        "\n"
        "(1) -> Std.Println;\n"
        "(2) -> Std.Println;\n");
}

TEST_CASE("Formatter keeps a let's doc comments attached", "[formatter]") {
  const std::string source =
      "import Std;\n"
      "\n"
      "// Adds one.\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
      "// A section comment.\n"
      "\n"
      "let Dec(x: Int64): Int64 = (x, 1) -> Std.Subtract;\n";
  const std::string formatted = format(source);
  CHECK(formatted == source);

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(formatted, "formatter_test.fleaux");
  REQUIRE(parsed.has_value());
  const auto& inc = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[1]);
  const auto& dec = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[2]);
  CHECK(inc.doc_comments == std::vector<std::string>{"Adds one."});
  CHECK(dec.doc_comments.empty());
}

TEST_CASE("Formatter lays out blocks and closures", "[formatter]") {
  CHECK(format("let V: Int64 = {let a: Int64 = 1; (a, 2) -> Std.Add;};") ==
        "let V: Int64 = { let a: Int64 = 1; (a, 2) -> Std.Add; };\n");
  CHECK(format("let V: Int64 = {let a: Int64 = 1; (a, 2) -> Std.Add;};", 30) ==
        "let V: Int64 = {\n"
        "  let a: Int64 = 1;\n"
        "  (a, 2) -> Std.Add;\n"
        "};\n");
  CHECK(format("(1) -> (x: Int64): Int64 = (x, 1) -> Std.Add -> Std.Println;") ==
        "(1) -> (x: Int64): Int64 = (x, 1) -> Std.Add -> Std.Println;\n");
  CHECK(format("(1) -> <T>(x: T): T = x;") == "(1) -> <T>(x: T): T = x;\n");
}

TEST_CASE("Formatter keeps CRLF line endings", "[formatter]") {
  CHECK(format("import Std;\r\n(1)->Std.Println;\r\n") == "import Std;\r\n(1) -> Std.Println;\r\n");
}

TEST_CASE("Formatter returns a parse error for source that does not parse", "[formatter]") {
  const auto formatted = format_source("let X: Int64 = ;", "formatter_test.fleaux");
  REQUIRE_FALSE(formatted.has_value());
  CHECK_FALSE(formatted.error().message.empty());
}

TEST_CASE("Formatter formats every sample and the standard library", "[formatter][samples]") {
  const std::filesystem::path repo_root(FLEAUX_REPO_ROOT);
  std::vector<std::filesystem::path> files{repo_root / "stdlib" / "Std.fleaux"};
  for (const auto& entry : std::filesystem::directory_iterator(repo_root / "samples")) {
    if (entry.path().extension() == ".fleaux") {
      files.push_back(entry.path());
    }
  }
  REQUIRE(files.size() > 1);

  for (const auto& path : files) {
    INFO(path.string());
    // format() requires success, which includes parsing back to the same program, and a stable second pass.
    (void)format(read_file(path));
  }
}

TEST_CASE("Formatter keeps native function declarations", "[formatter][native]") {
  CHECK(format("let Ext.Add(a:Int64,b:Int64):Int64::__native__;") ==
        "let Ext.Add(a: Int64, b: Int64): Int64 :: __native__;\n");
}

TEST_CASE("Formatter re-indents multi-line raw strings without changing them", "[formatter][raw_string]") {
  // The literal stays on the '=' line; its lines move with the closing quotes, keeping their relative indentation and
  // their trailing spaces.
  CHECK(format("let S: String =\n"
               "      \"\"\"\n"
               "      a  \n"
               "\n"
               "        b\n"
               "      \"\"\";\n") ==
        "let S: String = \"\"\"\n"
        "  a  \n"
        "\n"
        "    b\n"
        "  \"\"\";\n");
  CHECK(format("\"\"\"one line\"\"\"->Std.Println;") == "\"\"\"one line\"\"\" -> Std.Println;\n");
}

TEST_CASE("Formatter breaks long type argument lists one type a line", "[formatter][types]") {
  // A nested list that fits stays on its line.
  CHECK(format("alias Game = Tuple<Float64, Float64, Tuple<Int64...>, Float64, Int64, RandomGenerator, Float64, Float64, "
               "Bool>;") ==
        "alias Game = Tuple<\n"
        "  Float64,\n"
        "  Float64,\n"
        "  Tuple<Int64...>,\n"
        "  Float64,\n"
        "  Int64,\n"
        "  RandomGenerator,\n"
        "  Float64,\n"
        "  Float64,\n"
        "  Bool\n"
        ">;\n");
  CHECK(format("alias Pair = Tuple<Int64, String>;") == "alias Pair = Tuple<Int64, String>;\n");
  // A function type breaks its parameter list the same way.
  CHECK(format("alias Callback = (Tuple<Int64, Int64, Int64>, Tuple<Float64, Float64, Float64>, Tuple<String, String>) "
               "=> Result<Int64, String>;") ==
        "alias Callback = (\n"
        "  Tuple<Int64, Int64, Int64>,\n"
        "  Tuple<Float64, Float64, Float64>,\n"
        "  Tuple<String, String>\n"
        ") => Result<Int64, String>;\n");
}

TEST_CASE("Formatter breaks a long signature's parameters before its return type", "[formatter][types]") {
  CHECK(format("let Std.Parallel.Map<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<U...>, Tuple<Int64, "
               "String>> :: __builtin__;") ==
        "let Std.Parallel.Map<T, U>(\n"
        "  items: Tuple<T...>,\n"
        "  func: (T) => U\n"
        "): Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n");
}
