#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

auto push_i64_const(fleaux::bytecode::Module& bytecode_module, const std::int64_t value) -> std::int64_t {
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{value});
  return static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
}

auto push_u64_const(fleaux::bytecode::Module& bytecode_module, const std::uint64_t value) -> std::int64_t {
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{value});
  return static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
}

}  // namespace

TEST_CASE("VM executes arithmetic bytecode and prints result", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  constexpr fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  if (!result.has_value()) { INFO(result.error().message); }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM Std.Printf does not append trailing newline", "[vm][printf]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintfBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Printf");
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

  if (!result.has_value()) { INFO(result.error().message); }
  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 + 2 = 3");
}

TEST_CASE("VM executes value-ref opcodes round-trip", "[vm][lifetime][value_ref]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  bytecode_module.builtin_names.emplace_back("Std.UnaryPlus");
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = 0},
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

TEST_CASE("VM execute invalidates escaped transient callable refs", "[vm][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  fleaux::bytecode::Module bytecode_module;
  bytecode_module.builtin_names.emplace_back("Std.UnaryPlus");
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = 0},
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"Hello, "}});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"VM"}});
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

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
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
  REQUIRE(output.str() == "13\nHello, VM\n256\nTrue\nFalse\nTrue\n");
}

TEST_CASE("VM executes native kSelect opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  bytecode_module.builtin_names = {
      "Std.UnaryMinus",
      "Std.UnaryPlus",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeBuiltinFuncRef, .operand = 1},
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

TEST_CASE("VM executes native kLoopCall opcode", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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

TEST_CASE("VM native kDiv by zero returns floating result", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c0 = push_i64_const(bytecode_module, 0);
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

TEST_CASE("VM native kMod by zero returns NaN-like result", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c0 = push_i64_const(bytecode_module, 0);
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
  const auto c0 = push_i64_const(bytecode_module, 0);
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

TEST_CASE("VM kPushConst preserves UInt64 constants", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.builtin_names = {
      "Std.Type",
      "Std.Println",
  };
  const auto c42u = push_u64_const(bytecode_module, 42U);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42u},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
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
  bytecode_module.builtin_names = {
      "Std.Type",
      "Std.Println",
  };
  const auto c40u = push_u64_const(bytecode_module, 40U);
  const auto c2u = push_u64_const(bytecode_module, 2U);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c40u},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2u},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "UInt64\n");
}

TEST_CASE("VM native kAdd rejects mixed Int64 and UInt64 operands", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.builtin_names = {
      "Std.Type",
      "Std.Println",
  };
  const auto c_neg2 = push_i64_const(bytecode_module, -2);
  const auto c5u = push_u64_const(bytecode_module, 5U);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c_neg2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5u},
      {.opcode = fleaux::bytecode::Opcode::kAdd, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Add", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c5 = push_i64_const(bytecode_module, 5);
  bytecode_module.builtin_names = {
      "Std.Add",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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

TEST_CASE("VM kCallBuiltin falls back for unported builtin", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.builtin_names = {"Std.Println"};
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM strict mode executes native Std.Println", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.builtin_names = {"Std.Println"};
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin uses native dispatch for comparison and logical builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c8 = push_i64_const(bytecode_module, 8);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  bytecode_module.builtin_names = {"Std.LessThan", "Std.And", "Std.Println"};
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c8},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin uses native dispatch for unary builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  bytecode_module.builtin_names = {
      "Std.UnaryMinus",
      "Std.UnaryPlus",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
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

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Select", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  bytecode_module.builtin_names = {
      "Std.Select",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Branch", "[vm]") {
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
  bytecode_module.builtin_names = {
      "Std.Branch",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size());
  bytecode_module.builtin_names.emplace_back("Std.Println");
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

TEST_CASE("VM executes kMakeClosureRef for inline closure callables", "[vm]") {
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
  bytecode_module.builtin_names = {
      "Std.Apply",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c32},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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

TEST_CASE("VM reports too few arguments for inline closure callable", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);

  fleaux::bytecode::FunctionDef sum2;
  sum2.name = "__closure_sum2";
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
  bytecode_module.builtin_names = {"Std.Apply"};

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeClosureRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "native builtin 'Std.Apply' threw: too few arguments for inline closure");
}

TEST_CASE("VM executes Std.Match with wildcard closure case", "[vm]") {
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
  bytecode_module.builtin_names = {
      "Std.Match",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cWildcard},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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

TEST_CASE("VM executes Std.Match with predicate pattern case", "[vm]") {
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
  bytecode_module.builtin_names = {
      "Std.Match",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c6},

      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cWildcard},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
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

TEST_CASE("VM strict mode executes native Std.Result builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"boom"}});
  const auto cBoom = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  bytecode_module.builtin_names = {
      "Std.Result.Ok",    "Std.Result.IsOk",      "Std.Result.Unwrap", "Std.Result.Err",
      "Std.Result.IsErr", "Std.Result.UnwrapErr", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBoom},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},
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

TEST_CASE("VM kCallBuiltin uses native dispatch for more arithmetic/logical builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{false});
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c4 = push_i64_const(bytecode_module, 4);
  const auto c3 = push_i64_const(bytecode_module, 3);
  const auto c7 = push_i64_const(bytecode_module, 7);

  bytecode_module.builtin_names = {
      "Std.Subtract", "Std.Multiply", "Std.Or", "Std.Equal", "Std.NotEqual", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
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

TEST_CASE("VM kCallBuiltin uses native dispatch for apply/wrap/unwrap/to_num", "[vm]") {
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

  bytecode_module.builtin_names = {
      "Std.Apply", "Std.Wrap", "Std.Unwrap", "Std.ToNum", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c41},
      {.opcode = fleaux::bytecode::Opcode::kMakeUserFuncRef, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
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

TEST_CASE("VM kCallBuiltin uses native dispatch for tuple/math helper builtins", "[vm]") {
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

  bytecode_module.builtin_names = {
      "Std.Length", "Std.ElementAt", "Std.Take",       "Std.Drop",
      "Std.Slice",  "Std.Math.Sqrt", "Std.Math.Clamp", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      // Std.Length: pass the 3-tuple directly (Option B — no 1-element wrapper).
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c30},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c5},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 6},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n20\n10 20\n30\n20 30\n3\n5\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.ToString and Std.String helpers", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c42 = push_i64_const(bytecode_module, 42);
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
  const auto cDash = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
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

  bytecode_module.builtin_names = {
      "Std.ToString",          "Std.String.Upper",    "Std.String.Lower",   "Std.String.Trim",
      "Std.String.Split",      "Std.String.Join",     "Std.String.Replace", "Std.String.Contains",
      "Std.String.StartsWith", "Std.String.EndsWith", "Std.String.Length",  "Std.String.TrimStart",
      "Std.String.TrimEnd",    "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cHello},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMixed},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrim},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cCsv},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cB},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBc},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cCsv},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cComma},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cUnderscore},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 6},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cB},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAb},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cBc},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 9},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbcd},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 10},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrimStartOnly},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 11},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTrimEndOnly},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 12},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\nHELLO\nmixed\ntrim me\na b c\nabc,b,bc\na_b_c\nTrue\nTrue\nTrue\n4\nleft\nright\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Path and Std.OS helpers", "[vm]") {
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

  bytecode_module.builtin_names = {
      "Std.Path.Join",  "Std.Path.Basename", "Std.Path.Extension",     "Std.Path.Stem",         "Std.Path.Exists",
      "Std.Path.IsDir", "Std.OS.IsLinux",    "Std.Path.WithExtension", "Std.Path.WithBasename", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cTmp},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDot},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDot},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 6},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cLogExt},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFull},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cOtherBin},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "/tmp/file.txt\nfile.txt\n.txt\nfile\nTrue\nTrue\nTrue\n/tmp/file.log\n/tmp/other.bin\n");
}

TEST_CASE("VM strict mode executes native Std.String and Std.Path builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc"}});
  const auto cAbc = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp/file.txt"}});
  const auto cPath = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"log"}});
  const auto cLog = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  bytecode_module.builtin_names = {
      "Std.String.Upper",
      "Std.Path.WithExtension",
      "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cAbc},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cPath},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cLog},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "ABC\n/tmp/file.log\n");
}

TEST_CASE("VM strict mode executes native Std.OS, Std.Tuple, and Std.Dict builtins", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c1 = push_i64_const(bytecode_module, 1);
  const auto c2 = push_i64_const(bytecode_module, 2);
  const auto c3 = push_i64_const(bytecode_module, 3);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);

  bytecode_module.builtin_names = {
      "Std.OS.IsLinux", "Std.Tuple.Append", "Std.Dict.Create", "Std.Dict.Set", "Std.Dict.Get", "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
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

TEST_CASE("VM strict mode executes native Std.OS env and Std.File/Std.Dir builtins", "[vm]") {
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

  bytecode_module.builtin_names = {
      "Std.OS.SetEnv",     "Std.OS.Env",      "Std.OS.UnsetEnv", "Std.Dir.Create", "Std.File.WriteText",
      "Std.File.ReadText", "Std.File.Delete", "Std.Dir.Delete",  "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvVal},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cEnvKey},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDir},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cHello},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cFile},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 6},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cDir},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 7},
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

TEST_CASE("VM native Std.Path.Join reports native error prefix", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp"}});
  bytecode_module.builtin_names = {"Std.Path.Join"};
  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "native builtin 'Std.Path.Join' threw: PathJoin expects at least 2 arguments");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Tuple and Std.Dict helpers", "[vm]") {
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

  bytecode_module.builtin_names = {
      "Std.Tuple.Append", "Std.Tuple.Prepend", "Std.Tuple.Contains", "Std.Tuple.Zip",
      "Std.Dict.Create",  "Std.Dict.Set",      "Std.Dict.Get",       "Std.Dict.GetDefault",
      "Std.Dict.Length",  "Std.Dict.Keys",     "Std.Dict.Values",    "Std.Println",
  };
  const auto kPrintBuiltin = static_cast<std::int64_t>(bytecode_module.builtin_names.size() - 1);

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c1},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c3},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c4},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 4},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c9},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 5},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cKey},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 6},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 8},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 9},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kDup, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 1},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 10},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cMissing},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c42},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 3},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 7},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = kPrintBuiltin},

      {.opcode = fleaux::bytecode::Opcode::kPop, .operand = 0},

      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n0 1 2\nTrue\n(1, 3) (2, 4)\n9\n1\nk\n9\n42\n");
}

TEST_CASE("VM native Std.ElementAt rejects fractional index", "[vm]") {
  fleaux::bytecode::Module bytecode_module;
  const auto c10 = push_i64_const(bytecode_module, 10);
  const auto c20 = push_i64_const(bytecode_module, 20);
  bytecode_module.constants.push_back(fleaux::bytecode::ConstValue{1.5});
  const auto cIdx = static_cast<std::int64_t>(bytecode_module.constants.size() - 1);
  bytecode_module.builtin_names = {"Std.ElementAt"};

  bytecode_module.instructions = {
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c10},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = c20},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kPushConst, .operand = cIdx},
      {.opcode = fleaux::bytecode::Opcode::kBuildTuple, .operand = 2},
      {.opcode = fleaux::bytecode::Opcode::kCallBuiltin, .operand = 0},
      {.opcode = fleaux::bytecode::Opcode::kHalt, .operand = 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(bytecode_module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("expects an integer value") != std::string::npos);
}
