#pragma once

// The allocator for the runtime's heap objects: tuples, strings, dicts with their entries, function values, file
// handles, and tasks.
//
// Blocks of up to kMaxBlockBytes come from kClassCount size classes, kGranule bytes apart; bigger ones come from
// operator new. Each thread keeps a free list for each size class. Allocating pops a block from the calling thread's
// list, and freeing pushes the block onto the list of the thread that frees it, whichever thread allocated it. A list
// that runs empty takes a batch of blocks from a depot the threads share, or cuts a new chunk into blocks; a list that
// grows past its limit gives half of its blocks to the depot; a thread's lists go to the depot when the thread ends.
//
// Chunks are never freed, and the size class of a block follows from the size handed back with it alone. So any copy of
// the allocator can free a block that another copy allocated: the host and each extension, which compiles these
// headers and links a copy of its own, share values freely.
//
// Debug builds keep a check in the kGranule bytes in front of each block: the size it was allocated with, and whether
// it is live. Freeing a block with another size, or freeing it twice, aborts. A Debug library and a Release library
// therefore must not share values. With FLEAUX_HEAP_SYSTEM_ALLOCATOR (the AddressSanitizer and ThreadSanitizer
// builds), every block comes from operator new, so that the sanitizers see each allocation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "fleaux/common/thread_slot.hpp"

namespace fleaux::runtime::heap {

inline constexpr std::size_t kGranule = 16U;
inline constexpr std::size_t kClassCount = 16U;
inline constexpr std::size_t kMaxBlockBytes = kGranule * kClassCount;
inline constexpr std::size_t kChunkBytes = std::size_t{64U} * 1024U;

// Every block is aligned to this.
inline constexpr std::size_t kAlignment = kGranule;

#if defined(NDEBUG)
inline constexpr std::size_t kCheckBytes = 0U;
#else
inline constexpr std::size_t kCheckBytes = kGranule;
#endif

// The size class of a block of `bytes`, or kClassCount for one that comes from operator new.
[[nodiscard]] constexpr auto size_class_of(const std::size_t bytes) noexcept -> std::size_t {
  return bytes != 0U && bytes <= kMaxBlockBytes - kCheckBytes ? (bytes + kCheckBytes - 1U) / kGranule : kClassCount;
}

namespace detail {

struct FreeBlock {
  FreeBlock* next;
};

struct FreeList {
  FreeBlock* head{nullptr};
  std::size_t count{0U};
};

struct ThreadLists {
  std::array<FreeList, kClassCount> lists{};
};

[[nodiscard]] constexpr auto block_bytes(const std::size_t size_class) noexcept -> std::size_t {
  return (size_class + 1U) * kGranule;
}

// The most blocks a thread's list keeps: one chunk's worth.
inline constexpr auto kListLimits = [] {
  std::array<std::size_t, kClassCount> limits{};
  for (std::size_t size_class = 0U; size_class < kClassCount; ++size_class) {
    limits[size_class] = kChunkBytes / block_bytes(size_class);
  }
  return limits;
}();

// Each thread's lists. Their cleanup gives them to the depot when the thread ends, and when this library unloads or the
// process exits.
extern common::ThreadSlot g_lists_slot;

// Makes the calling thread's lists; null once the thread is ending or the slot has shut down.
[[nodiscard]] auto make_thread_lists() noexcept -> ThreadLists*;

// The calling thread's lists, made on its first call. Null once the thread is ending or the allocator has shut down
// (its library is unloading, or the process is exiting); blocks then go to and from the depot.
[[nodiscard]] inline auto thread_lists() noexcept -> ThreadLists* {
  if (void* const lists = g_lists_slot.get(); lists != nullptr) {
    return static_cast<ThreadLists*>(lists);
  }
  return make_thread_lists();
}

// Fills the empty `list` with a batch from the depot, or with a new chunk, and returns one block of the class.
[[nodiscard]] auto refill(FreeList& list, std::size_t size_class) -> void*;

// Gives half of `list`, which is over its limit, to the depot.
void spill(FreeList& list, std::size_t size_class) noexcept;

// A block of the class from the depot, and one given back to it, for a thread without lists.
[[nodiscard]] auto take_shared(std::size_t size_class) -> void*;
void give_shared(void* block, std::size_t size_class) noexcept;

// Reports a block freed with the wrong size or freed twice, and aborts.
[[noreturn]] void report_bad_free(const void* block, std::size_t bytes) noexcept;

// For tests: the blocks of a size class in the calling thread's list and in the depot, and the chunks cut so far.
[[nodiscard]] auto thread_list_count(std::size_t size_class) noexcept -> std::size_t;
[[nodiscard]] auto depot_count(std::size_t size_class) noexcept -> std::size_t;
[[nodiscard]] auto chunk_count() noexcept -> std::size_t;

[[nodiscard]] inline auto take(const std::size_t size_class) -> void* {
  if (ThreadLists* const lists = thread_lists(); lists != nullptr) {
    FreeList& list = lists->lists[size_class];
    if (FreeBlock* const head = list.head; head != nullptr) {
      list.head = head->next;
      --list.count;
      return head;
    }
    return refill(list, size_class);
  }
  return take_shared(size_class);
}

inline void give(void* block, const std::size_t size_class) noexcept {
  if (ThreadLists* const lists = thread_lists(); lists != nullptr) {
    FreeList& list = lists->lists[size_class];
    list.head = ::new (block) FreeBlock{list.head};
    if (++list.count > kListLimits[size_class]) {
      spill(list, size_class);
    }
    return;
  }
  give_shared(block, size_class);
}

// The Debug check in front of a block: the size it was allocated with, and a mark that says whether it is live.
inline constexpr std::uint64_t kLiveMark = 0x4C49'5645'424C'4F43ULL;
inline constexpr std::uint64_t kFreedMark = 0x4652'4545'424C'4F43ULL;

[[nodiscard]] inline auto checked_block(void* start, const std::size_t bytes) noexcept -> void* {
  if constexpr (kCheckBytes == 0U) {
    (void)bytes;
    return start;
  } else {
    const std::uint64_t size = bytes;
    std::memcpy(start, &size, sizeof size);
    std::memcpy(static_cast<std::byte*>(start) + sizeof size, &kLiveMark, sizeof kLiveMark);
    return static_cast<std::byte*>(start) + kCheckBytes;
  }
}

// Whether `block` is a live block of `bytes`, by its Debug check; always true in Release builds.
[[nodiscard]] inline auto is_live_block(const void* block, const std::size_t bytes) noexcept -> bool {
  if constexpr (kCheckBytes == 0U) {
    (void)block;
    (void)bytes;
    return true;
  } else {
    const auto* const start = static_cast<const std::byte*>(block) - kCheckBytes;
    std::uint64_t size = 0U;
    std::uint64_t mark = 0U;
    std::memcpy(&size, start, sizeof size);
    std::memcpy(&mark, start + sizeof size, sizeof mark);
    return mark == kLiveMark && size == bytes;
  }
}

// The start of a block being freed, after its Debug check.
[[nodiscard]] inline auto block_start(void* block, const std::size_t bytes) noexcept -> void* {
  if constexpr (kCheckBytes == 0U) {
    (void)bytes;
    return block;
  } else {
    if (!is_live_block(block, bytes)) {
      report_bad_free(block, bytes);
    }
    void* const start = static_cast<std::byte*>(block) - kCheckBytes;
    std::memcpy(static_cast<std::byte*>(start) + sizeof(std::uint64_t), &kFreedMark, sizeof kFreedMark);
    return start;
  }
}

}  // namespace detail

// A block of `bytes`, aligned to kAlignment.
[[nodiscard]] inline auto allocate(const std::size_t bytes) -> void* {
#if defined(FLEAUX_HEAP_SYSTEM_ALLOCATOR)
  return ::operator new(bytes);
#else
  if constexpr (kCheckBytes != 0U) {
    if (bytes > std::numeric_limits<std::size_t>::max() - kCheckBytes) {
      throw std::bad_alloc{};
    }
  }
  const std::size_t size_class = size_class_of(bytes);
  void* const start = size_class < kClassCount ? detail::take(size_class) : ::operator new(bytes + kCheckBytes);
  return detail::checked_block(start, bytes);
#endif
}

// Frees a block that allocate(bytes) gave, with the same `bytes`.
inline void deallocate(void* block, const std::size_t bytes) noexcept {
#if defined(FLEAUX_HEAP_SYSTEM_ALLOCATOR)
  ::operator delete(block, bytes);
#else
  void* const start = detail::block_start(block, bytes);
  if (const std::size_t size_class = size_class_of(bytes); size_class < kClassCount) {
    detail::give(start, size_class);
  } else {
    ::operator delete(start, bytes + kCheckBytes);
  }
#endif
}

// A standard allocator for the containers inside heap objects (a dict's entries and index).
template <typename T>
class Allocator {
public:
  using value_type = T;

  static_assert(alignof(T) <= kAlignment, "heap blocks are aligned to 16 bytes");

  Allocator() noexcept = default;

  template <typename Other>
  Allocator(const Allocator<Other>& /*other*/) noexcept {}

  [[nodiscard]] auto allocate(const std::size_t count) -> T* {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length{};
    }
    return static_cast<T*>(heap::allocate(count * sizeof(T)));
  }

  void deallocate(T* items, const std::size_t count) noexcept { heap::deallocate(items, count * sizeof(T)); }

  friend auto operator==(const Allocator& /*lhs*/, const Allocator& /*rhs*/) noexcept -> bool { return true; }
};

}  // namespace fleaux::runtime::heap
