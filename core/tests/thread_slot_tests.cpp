#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/common/thread_slot.hpp"

namespace {

// What a slot's cleanup saw: the pointers it ran on, whether get() was already null each time, and whether set() was
// refused.
struct CleanupRecord {
  std::mutex mutex;
  std::vector<void*> cleaned;
  bool slot_was_null = true;
  bool set_was_refused = true;
};

// Each test has a slot of its own: a slot lives as long as the program.
template <int Slot>
CleanupRecord g_record;

template <int Slot>
void record_cleanup(void* value) noexcept;

template <int Slot>
constinit fleaux::common::ThreadSlot g_slot{&record_cleanup<Slot>};

template <int Slot>
void record_cleanup(void* value) noexcept {
  CleanupRecord& record = g_record<Slot>;
  const std::scoped_lock lock(record.mutex);
  record.cleaned.push_back(value);
  record.slot_was_null = record.slot_was_null && g_slot<Slot>.get() == nullptr;
  record.set_was_refused = record.set_was_refused && !g_slot<Slot>.set(value);
}

template <int Slot>
auto was_cleaned(const void* value) -> bool {
  CleanupRecord& record = g_record<Slot>;
  const std::scoped_lock lock(record.mutex);
  return std::ranges::find(record.cleaned, value) != record.cleaned.end();
}

}  // namespace

TEST_CASE("A thread slot holds one pointer for each thread", "[common][thread_slot]") {
  auto& slot = g_slot<0>;
  int main_value = 0;
  int worker_value = 0;
  CHECK(slot.get() == nullptr);
  REQUIRE(slot.set(&main_value));
  CHECK(slot.get() == &main_value);

  void* worker_saw_first = &main_value;
  void* worker_saw_after_set = nullptr;
  bool worker_set = false;
  std::thread([&] {
    worker_saw_first = slot.get();
    worker_set = slot.set(&worker_value);
    worker_saw_after_set = slot.get();
  }).join();

  CHECK(worker_saw_first == nullptr);
  CHECK(worker_set);
  CHECK(worker_saw_after_set == &worker_value);
  CHECK(slot.get() == &main_value);
}

TEST_CASE("A thread slot's cleanup runs on a thread's pointer when the thread ends", "[common][thread_slot]") {
  auto& slot = g_slot<1>;
  int worker_value = 0;
  bool worker_set = false;
  std::thread([&] { worker_set = slot.set(&worker_value); }).join();

  REQUIRE(worker_set);
  CHECK(was_cleaned<1>(&worker_value));
  const std::scoped_lock lock(g_record<1>.mutex);
  CHECK(g_record<1>.cleaned.size() == 1U);
  // While the cleanup ran, the thread's slot was already empty and refused a new pointer.
  CHECK(g_record<1>.slot_was_null);
  CHECK(g_record<1>.set_was_refused);
}

TEST_CASE("A thread slot that shuts down is empty on every thread", "[common][thread_slot]") {
  auto& slot = g_slot<2>;
  int main_value = 0;
  int worker_value = 0;
  REQUIRE(slot.set(&main_value));

  std::mutex mutex;
  std::condition_variable changed;
  bool worker_ready = false;
  bool worker_set = false;
  bool shut = false;
  bool worker_saw_null = false;
  bool worker_was_refused = false;
  std::thread worker([&] {
    const bool set = slot.set(&worker_value);
    std::unique_lock lock(mutex);
    worker_set = set;
    worker_ready = true;
    changed.notify_all();
    changed.wait(lock, [&] { return shut; });
    worker_saw_null = slot.get() == nullptr;
    worker_was_refused = !slot.set(&worker_value);
  });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return worker_ready; });
  }

  slot.shut_down();
  CHECK(slot.get() == nullptr);
  CHECK_FALSE(slot.set(&main_value));
#if defined(_WIN32)
  // Windows runs the cleanup on every thread's pointer when the slot shuts down.
  CHECK(was_cleaned<2>(&main_value));
  CHECK(was_cleaned<2>(&worker_value));
#endif

  {
    const std::scoped_lock lock(mutex);
    shut = true;
  }
  changed.notify_all();
  worker.join();

  CHECK(worker_set);
  CHECK(worker_saw_null);
  CHECK(worker_was_refused);
  CHECK(was_cleaned<2>(&worker_value));
}
