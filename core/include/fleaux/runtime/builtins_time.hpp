#pragma once
// The clocks, and waiting. Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.

#include <chrono>
#include <ratio>
#include <thread>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// The milliseconds since 1970-01-01 UTC, from the system clock, which can be set back.
[[nodiscard]] inline auto TimeNow(const Arguments& /*args*/) -> Value {
  return make_int(
      std::chrono::duration_cast<std::chrono::duration<Int, std::milli>>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Nanoseconds since some fixed moment, from a clock that only goes forward: for measuring durations.
[[nodiscard]] inline auto TimeMonotonic(const Arguments& /*args*/) -> Value {
  return make_int(
      std::chrono::duration_cast<std::chrono::duration<Int, std::nano>>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// arg = ms -> () after waiting at least ms milliseconds. One sleep_for can end early (under load, one ended after 1 ms of
// a 5 ms sleep with MinGW), so it sleeps again until the steady clock, which Std.Time.Monotonic reads, is past the end.
[[nodiscard]] inline auto TimeSleep(Value arg) -> Value {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(count_index(arg, "ms"));
  for (auto now = std::chrono::steady_clock::now(); now < end; now = std::chrono::steady_clock::now()) {
    std::this_thread::sleep_for(end - now);
  }
  return make_tuple();
}

}  // namespace fleaux::runtime
