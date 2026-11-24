#pragma once

// A pointer for each thread, with a cleanup function that runs on a thread's pointer when the thread ends.
//
// On Windows the pointer is found with TlsGetValue, because MinGW emulates thread_local and every access of an emulated
// one takes a lock. A fiber-local slot holds the pointer too, for its callback: Windows calls it when the thread ends,
// and for every thread when the slot is freed. Each library's slots are freed when the library unloads or the process
// exits, so a slot's cleanup also runs then, on every thread's pointer. Elsewhere each thread keeps the slots'
// pointers in a thread_local array, and a thread_local guard runs the cleanups when the thread ends.
//
// A slot lives as long as its library: declare it constinit at namespace scope. It is never destroyed, so code that
// runs while the process exits can still ask it, and gets null once it has shut down.

#include <atomic>
#include <cstdint>

namespace fleaux::common {

class ThreadSlot {
public:
  // Runs on a thread's pointer. When it runs, get() on that thread already returns null.
  using Cleanup = void (*)(void* value) noexcept;

  explicit constexpr ThreadSlot(const Cleanup cleanup) noexcept : cleanup_(cleanup) {}

  ThreadSlot(const ThreadSlot&) = delete;
  auto operator=(const ThreadSlot&) -> ThreadSlot& = delete;
  ThreadSlot(ThreadSlot&&) = delete;
  auto operator=(ThreadSlot&&) -> ThreadSlot& = delete;
  ~ThreadSlot() = default;

  // The calling thread's pointer: null before the thread sets one, once the thread's cleanup has run, and once the slot
  // has shut down.
  [[nodiscard]] auto get() const noexcept -> void*;

  // Sets the calling thread's pointer. False, with nothing set, once the thread's cleanup has run, once the slot has
  // shut down, or when the platform has no slot to give.
  [[nodiscard]] auto set(void* value) noexcept -> bool;

  // Makes get() null and set() false on every thread. On Windows it runs the cleanup on every thread's pointer, which no
  // other thread may be using; elsewhere each thread's cleanup still runs when the thread ends. The library shuts its
  // slots down when it unloads or the process exits.
  void shut_down() noexcept;

private:
  friend class ThreadSlotInternals;

  static constexpr std::uint32_t kNoIndex = 0xFFFF'FFFFU;

  Cleanup cleanup_;
  std::atomic<bool> shut_down_{false};
  // On Windows the TLS slot that get() reads, and kNoIndex before set-up and after shut-down; elsewhere the slot's index
  // in each thread's array.
  std::atomic<std::uint32_t> index_{kNoIndex};
#if defined(_WIN32)
  // The TLS slot, kept for the fiber-local callback after shut-down began, and the fiber-local slot.
  std::uint32_t lookup_slot_{kNoIndex};
  std::uint32_t ending_slot_{kNoIndex};
#endif
};

}  // namespace fleaux::common
