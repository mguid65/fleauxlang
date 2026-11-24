#include "fleaux/common/thread_slot.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fleaux::common {
namespace {

// Never destroyed: a slot can be set up, and shut down, while the process exits.
auto setup_mutex() -> std::mutex& {
  static std::mutex& mutex = *new std::mutex;
  return mutex;
}

}  // namespace

#if defined(_WIN32)

static_assert(sizeof(DWORD) == sizeof(std::uint32_t));
static_assert(TLS_OUT_OF_INDEXES == 0xFFFF'FFFFU && FLS_OUT_OF_INDEXES == 0xFFFF'FFFFU);

namespace {

// The TLS value of a thread whose cleanup has run.
constexpr auto kEnded = std::uintptr_t{1U};

// What the fiber-local slot holds for a thread: the slot and the thread's pointer.
struct Binding {
  const ThreadSlot* slot;
  void* value;
};

}  // namespace

class ThreadSlotInternals {
public:
  static void NTAPI end_thread(void* data) {
    const std::unique_ptr<Binding> binding(static_cast<Binding*>(data));
    const ThreadSlot& slot = *binding->slot;
    // On the thread that is ending, get() is null from now on, and set() refuses.
    if (TlsGetValue(slot.lookup_slot_) == binding->value) {
      TlsSetValue(slot.lookup_slot_, reinterpret_cast<void*>(kEnded));
    }
    if (binding->value != nullptr) {
      slot.cleanup_(binding->value);
    }
  }

  // Makes the slot's TLS and fiber-local slots, the first time any thread sets it.
  static auto set_up(ThreadSlot& slot) noexcept -> bool {
    const std::scoped_lock lock(setup_mutex());
    if (slot.index_.load(std::memory_order_relaxed) != ThreadSlot::kNoIndex) {
      return true;
    }
    if (slot.shut_down_.load(std::memory_order_relaxed)) {
      return false;
    }
    const DWORD lookup = TlsAlloc();
    if (lookup == TLS_OUT_OF_INDEXES) {
      return false;
    }
    const DWORD ending = FlsAlloc(&end_thread);
    if (ending == FLS_OUT_OF_INDEXES || !register_slot(slot)) {
      if (ending != FLS_OUT_OF_INDEXES) {
        FlsFree(ending);
      }
      TlsFree(lookup);
      return false;
    }
    slot.lookup_slot_ = lookup;
    slot.ending_slot_ = ending;
    slot.index_.store(lookup, std::memory_order_release);
    return true;
  }

private:
  // The slots this library has set up. Destroying the list, when the library unloads or the process exits, shuts them
  // down.
  class Registry {
  public:
    Registry() = default;
    Registry(const Registry&) = delete;
    auto operator=(const Registry&) -> Registry& = delete;
    Registry(Registry&&) = delete;
    auto operator=(Registry&&) -> Registry& = delete;

    ~Registry() {
      for (ThreadSlot* const slot : slots_) {
        slot->shut_down();
      }
    }

    void add(ThreadSlot& slot) { slots_.push_back(&slot); }

  private:
    std::vector<ThreadSlot*> slots_;
  };

  static auto register_slot(ThreadSlot& slot) noexcept -> bool {
    static Registry registry;
    try {
      registry.add(slot);
      return true;
    } catch (const std::bad_alloc&) {
      return false;
    }
  }
};

auto ThreadSlot::get() const noexcept -> void* {
  const std::uint32_t index = index_.load(std::memory_order_acquire);
  if (index == kNoIndex) {
    return nullptr;
  }
  void* const value = TlsGetValue(index);
  return reinterpret_cast<std::uintptr_t>(value) > kEnded ? value : nullptr;
}

auto ThreadSlot::set(void* value) noexcept -> bool {
  if (shut_down_.load(std::memory_order_acquire)) {
    return false;
  }
  if (index_.load(std::memory_order_acquire) == kNoIndex && !ThreadSlotInternals::set_up(*this)) {
    return false;
  }
  if (reinterpret_cast<std::uintptr_t>(TlsGetValue(lookup_slot_)) == kEnded) {
    return false;
  }
  if (auto* const binding = static_cast<Binding*>(FlsGetValue(ending_slot_)); binding != nullptr) {
    binding->value = value;
  } else {
    auto* const made = new (std::nothrow) Binding{.slot = this, .value = value};
    if (made == nullptr || FlsSetValue(ending_slot_, made) == FALSE) {
      delete made;
      return false;
    }
  }
  return TlsSetValue(lookup_slot_, value) != FALSE;
}

void ThreadSlot::shut_down() noexcept {
  const std::scoped_lock lock(setup_mutex());
  if (shut_down_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  if (index_.exchange(kNoIndex, std::memory_order_acq_rel) == kNoIndex) {
    return;
  }
  // Windows calls end_thread for every thread's binding, on this thread, before FlsFree returns.
  FlsFree(ending_slot_);
  TlsFree(lookup_slot_);
}

#else

namespace {

// The most slots one library sets up.
constexpr std::size_t kMaxSlots = 8U;

std::array<const ThreadSlot*, kMaxSlots> g_slots{};
std::uint32_t g_slot_count = 0U;

// Each thread's pointers, by slot index. The guard holds copies: MinGW's emulated thread_local, which this path also
// builds with, frees a thread's storage before it runs the thread's thread_local destructors.
thread_local std::array<void*, kMaxSlots> t_values{};
thread_local bool t_ended = false;

}  // namespace

class ThreadSlotInternals {
public:
  // Runs the cleanups of the thread's pointers when the thread ends.
  struct ThreadEnd {
    ThreadEnd() = default;
    ThreadEnd(const ThreadEnd&) = delete;
    auto operator=(const ThreadEnd&) -> ThreadEnd& = delete;
    ThreadEnd(ThreadEnd&&) = delete;
    auto operator=(ThreadEnd&&) -> ThreadEnd& = delete;

    ~ThreadEnd() {
      t_ended = true;
      for (std::size_t index = 0U; index < kMaxSlots; ++index) {
        if (void* const value = values[index]; value != nullptr) {
          t_values[index] = nullptr;
          values[index] = nullptr;
          g_slots[index]->cleanup_(value);
        }
      }
    }

    std::array<void*, kMaxSlots> values{};
  };

  // Gives the slot its index, the first time any thread sets it.
  static auto set_up(ThreadSlot& slot) noexcept -> bool {
    const std::scoped_lock lock(setup_mutex());
    if (slot.index_.load(std::memory_order_relaxed) != ThreadSlot::kNoIndex) {
      return true;
    }
    if (g_slot_count == kMaxSlots) {
      return false;
    }
    g_slots[g_slot_count] = &slot;
    slot.index_.store(g_slot_count++, std::memory_order_release);
    return true;
  }

  static auto thread_end() -> ThreadEnd& {
    thread_local ThreadEnd guard;
    return guard;
  }
};

auto ThreadSlot::get() const noexcept -> void* {
  const std::uint32_t index = index_.load(std::memory_order_acquire);
  if (index == kNoIndex || shut_down_.load(std::memory_order_relaxed)) {
    return nullptr;
  }
  return t_values[index];
}

auto ThreadSlot::set(void* value) noexcept -> bool {
  if (shut_down_.load(std::memory_order_acquire) || t_ended) {
    return false;
  }
  if (index_.load(std::memory_order_acquire) == kNoIndex && !ThreadSlotInternals::set_up(*this)) {
    return false;
  }
  const std::uint32_t index = index_.load(std::memory_order_acquire);
  ThreadSlotInternals::thread_end().values[index] = value;
  t_values[index] = value;
  return true;
}

void ThreadSlot::shut_down() noexcept { shut_down_.store(true, std::memory_order_release); }

#endif

}  // namespace fleaux::common
