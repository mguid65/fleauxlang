#include <filesystem>
#include <string>
#include <thread>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/value.hpp"

namespace {

using fleaux::embed::BindingContext;
using fleaux::embed::NativeBinding;
using fleaux::embed::NativeBindingRegistry;
using fleaux::embed::NativeInvokeResult;
using fleaux::embed::VmValue;

// The parameters of the native function, as the tuple the binding receives.
auto int_arg(const VmValue& args, const std::size_t index) -> fleaux::runtime::Int {
  return fleaux::runtime::as_int_value(fleaux::runtime::tuple_at(args, index));
}

void register_binding(NativeBindingRegistry& registry, std::string symbol,
                      fleaux::embed::NativeCallable callable) {
  const auto registered = registry.register_callable(NativeBinding{
      .symbol = std::move(symbol),
      .signature = fleaux::embed::BindingSignature{},
      .callable = std::move(callable),
  });
  REQUIRE(registered.has_value());
}

struct HostFixture {
  NativeBindingRegistry registry;
  std::string captured;

  auto run(const std::string_view source) -> fleaux::embed::VmResult {
    fleaux::embed::VmHostConfig config;
    config.binding_registry = &registry;
    config.stdout_sink = [this](const std::string_view text) { captured.append(text); };
    fleaux::embed::VmHost host(config);
    return host.run_source("native_call_test.fleaux", source);
  }
};

}  // namespace

TEST_CASE("A Fleaux program calls native functions through the host's bindings", "[embed][native]") {
  HostFixture fixture;
  register_binding(fixture.registry, "Ext.Add",
                   [](const BindingContext&, const VmValue& args) -> NativeInvokeResult {
                     return fleaux::runtime::make_int(int_arg(args, 0) + int_arg(args, 1));
                   });
  register_binding(fixture.registry, "Ext.Answer",
                   [](const BindingContext&, const VmValue& args) -> NativeInvokeResult {
                     // A native function without parameters receives an empty tuple.
                     REQUIRE(fleaux::runtime::as_tuple(args).size() == 0);
                     return fleaux::runtime::make_int(42);
                   });
  register_binding(fixture.registry, "Ext.Double",
                   [](const BindingContext&, const VmValue& args) -> NativeInvokeResult {
                     return fleaux::runtime::make_int(2 * int_arg(args, 0));
                   });

  // Ext.Double is also passed as a function value.
  const auto result = fixture.run(
      "import Std;\n"
      "let Ext.Add(a: Int64, b: Int64): Int64 :: __native__;\n"
      "let Ext.Answer(): Int64 :: __native__;\n"
      "let Ext.Double(x: Int64): Int64 :: __native__;\n"
      "(2, 3) -> Ext.Add -> Std.Println;\n"
      "() -> Ext.Answer -> Std.Println;\n"
      "(21, Ext.Double) -> Std.Apply -> Std.Println;\n");
  INFO((result.has_value() ? std::string{} : result.error().message));
  REQUIRE(result.has_value());
  CHECK(fixture.captured == "5\n42\n42\n");
}

TEST_CASE("A program whose native function no extension provides stops before it runs", "[embed][native]") {
  HostFixture fixture;
  const auto result = fixture.run(
      "import Std;\n"
      "let Ext.Missing(a: Int64): Int64 :: __native__;\n"
      "\"started\" -> Std.Println;\n"
      "(1) -> Ext.Missing -> Std.Println;\n");
  REQUIRE_FALSE(result.has_value());
  CHECK_THAT(result.error().message,
             Catch::Matchers::ContainsSubstring("Native function 'Ext.Missing' is not provided by any loaded extension"));
  CHECK(fixture.captured.empty());
}

TEST_CASE("The VM checks what a native function returns against its declared type", "[embed][native]") {
  HostFixture fixture;
  register_binding(fixture.registry, "Ext.Count",
                   [](const BindingContext&, const VmValue&) -> NativeInvokeResult {
                     return fleaux::runtime::make_string("not a count");
                   });
  const auto result = fixture.run(
      "import Std;\n"
      "let Ext.Count(): Int64 :: __native__;\n"
      "() -> Ext.Count -> Std.Println;\n");
  REQUIRE_FALSE(result.has_value());
  CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring("native function 'Ext.Count' returned"));
  CHECK_THAT(result.error().message, Catch::Matchers::ContainsSubstring("declared to return Int64"));
}

TEST_CASE("A native function's error becomes a runtime error", "[embed][native]") {
  HostFixture fixture;
  register_binding(fixture.registry, "Ext.Fail",
                   [](const BindingContext&, const VmValue&) -> NativeInvokeResult {
                     return tl::unexpected(fleaux::embed::HostError{
                         .category = fleaux::embed::HostErrorCategory::kBinding,
                         .message = "the device is gone",
                     });
                   });
  const auto result = fixture.run(
      "import Std;\n"
      "let Ext.Fail(): Null :: __native__;\n"
      "() -> Ext.Fail -> Std.Println;\n");
  REQUIRE_FALSE(result.has_value());
  CHECK_THAT(result.error().message,
             Catch::Matchers::ContainsSubstring("native function 'Ext.Fail' failed: the device is gone"));
}

TEST_CASE("Runs on one thread reuse its stacks", "[embed][native]") {
  // Runtime::execute takes the thread's operand and frame stacks, which keep their capacity from one run to the next.
  const std::string depth_source =
      "import Std;\n"
      "let Depth(n: Int64): Int64 =\n"
      "    (n,\n"
      "     (0, (): Int64 = 0),\n"
      "     (_, (): Int64 = (((n, 1) -> Std.Subtract) -> Depth, 1) -> Std.Add)\n"
      "    ) -> Std.Match;\n";

  SECTION("The same program prints the same output each run") {
    HostFixture fixture;
    for (int run = 0; run < 3; ++run) {
      const auto result = fixture.run(depth_source + "(10) -> Depth -> Std.Println;\n");
      REQUIRE(result.has_value());
    }
    CHECK(fixture.captured == "10\n10\n10\n");
  }

  SECTION("A run recurses deeper than the runs before it") {
    HostFixture fixture;
    for (const char* depth : {"10", "3000", "10"}) {
      const auto result = fixture.run(depth_source + "(" + depth + ") -> Depth -> Std.Println;\n");
      REQUIRE(result.has_value());
    }
    CHECK(fixture.captured == "10\n3000\n10\n");
  }

  SECTION("A run inside a native call during another run") {
    HostFixture fixture;
    register_binding(fixture.registry, "Ext.Inner",
                     [&depth_source](const BindingContext&, const VmValue& args) -> NativeInvokeResult {
                       HostFixture inner;
                       const auto result =
                           inner.run(depth_source + "(" + std::to_string(int_arg(args, 0)) + ") -> Depth -> Std.Println;\n");
                       REQUIRE(result.has_value());
                       return fleaux::runtime::make_string(inner.captured);
                     });
    const auto result = fixture.run(depth_source +
                                    "let Ext.Inner(n: Int64): String :: __native__;\n"
                                    "let Outer(n: Int64): String = (n, (n) -> Depth) -> Std.Add -> Ext.Inner;\n"
                                    "(20) -> Outer -> Std.Println;\n"
                                    "(5) -> Depth -> Std.Println;\n");
    INFO((result.has_value() ? std::string{} : result.error().message));
    REQUIRE(result.has_value());
    // The inner run printed "40\n", which the outer one prints with a newline of its own.
    CHECK(fixture.captured == "40\n\n5\n");
  }

  SECTION("A value left in a frame's locals is released when the frame returns") {
    // The runtime lists every file handle that exists. Hold never reads its parameter, so the handle stays in the
    // frame's locals until the frame returns.
    const auto open_handles = [] {
      std::size_t count = 0U;
      fleaux::runtime::open_files().for_each([&count](const fleaux::runtime::FileHandleObject&) { ++count; });
      return count;
    };
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "fleaux_frame_locals_probe.txt";
    const std::size_t handles_before = open_handles();
    HostFixture fixture;
    const auto result = fixture.run(
        "import Std;\n"
        "let Hold(handle: FileHandle): Int64 = 1;\n"
        "(\"" + path.generic_string() + "\", \"w\") -> Std.File.Open -> Hold -> Std.Println;\n");
    INFO((result.has_value() ? std::string{} : result.error().message));
    REQUIRE(result.has_value());
    CHECK(fixture.captured == "1\n");
    CHECK(open_handles() == handles_before);
    std::filesystem::remove(path);
  }
}

TEST_CASE("Each thread's runs keep their own streams and execution state", "[embed][native][thread]") {
  SECTION("Runs on two threads at once print only to their own output") {
    const auto program = [](const std::string& word) {
      return "import Std;\n"
             "let Say(n: Int64): Int64 = { \"" + word + "\" -> Std.Println; (n, 1) -> +; };\n"
             "(0, (n: Int64): Bool = (n, 200) -> <, Say) -> Std.Loop -> Std.Println;\n";
    };
    const auto expected = [](const std::string& word) {
      std::string run;
      for (int line = 0; line < 200; ++line) {
        run += word + "\n";
      }
      run += "200\n";
      std::string all;
      for (int repeat = 0; repeat < 5; ++repeat) {
        all += run;
      }
      return all;
    };
    // Catch2's checks are not thread-safe, so each thread records what it saw and the checks come after.
    const auto run_five_times = [&program](const std::string& word, std::string& captured, bool& all_ran) {
      HostFixture fixture;
      all_ran = true;
      for (int repeat = 0; repeat < 5; ++repeat) {
        all_ran = fixture.run(program(word)).has_value() && all_ran;
      }
      captured = fixture.captured;
    };
    std::string left;
    std::string right;
    bool left_ran = false;
    bool right_ran = false;
    std::thread left_thread([&] { run_five_times("left", left, left_ran); });
    std::thread right_thread([&] { run_five_times("right", right, right_ran); });
    left_thread.join();
    right_thread.join();

    CHECK(left_ran);
    CHECK(right_ran);
    CHECK(left == expected("left"));
    CHECK(right == expected("right"));
  }

  SECTION("A task prints to the output of the run that spawned it") {
    // The task's worker enters the run's execution state and output stream for each job.
    HostFixture fixture;
    const auto result = fixture.run(
        "import Std;\n"
        "let Speak(n: Int64): Int64 = { \"from the task\" -> Std.Println; n; };\n"
        "((Speak, 7) -> Std.Task.Spawn,) -> Std.Task.AwaitAll -> Std.Println;\n");
    INFO((result.has_value() ? std::string{} : result.error().message));
    REQUIRE(result.has_value());
    CHECK(fixture.captured == "from the task\n(True, (7,))\n");
  }
}
