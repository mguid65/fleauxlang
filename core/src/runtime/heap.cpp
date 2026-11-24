#include "fleaux/runtime/heap.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fleaux::runtime::heap::detail {
namespace {

// The batches of blocks that threads gave back, for each size class. A shelf holds up to kShelfBatches batches; a
// batch given to a full shelf joins its last one.
constexpr std::size_t kShelfBatches = 64U;

struct Shelf {
  std::mutex mutex;
  std::array<FreeList, kShelfBatches> batches{};
  std::size_t batch_count{0U};
};

struct Depot {
  std::array<Shelf, kClassCount> shelves;
};

// Never destroyed: blocks freed while the process exits, after the threads' lists have gone, still go here.
auto depot() -> Depot& {
  static Depot& instance = *new Depot;
  return instance;
}

// The last block of a list that is not empty.
auto last_block(const FreeList& list) noexcept -> FreeBlock* {
  FreeBlock* block = list.head;
  while (block->next != nullptr) {
    block = block->next;
  }
  return block;
}

// Gives `batch`, which is not empty, to the depot.
void give_batch(const FreeList& batch, const std::size_t size_class) noexcept {
  Shelf& shelf = depot().shelves[size_class];
  const std::scoped_lock lock(shelf.mutex);
  if (shelf.batch_count < kShelfBatches) {
    shelf.batches[shelf.batch_count++] = batch;
    return;
  }
  FreeList& last = shelf.batches[kShelfBatches - 1U];
  last_block(batch)->next = last.head;
  last.head = batch.head;
  last.count += batch.count;
}

// A batch from the depot, or an empty list when it has none.
auto take_batch(const std::size_t size_class) noexcept -> FreeList {
  Shelf& shelf = depot().shelves[size_class];
  const std::scoped_lock lock(shelf.mutex);
  if (shelf.batch_count == 0U) {
    return {};
  }
  return shelf.batches[--shelf.batch_count];
}

std::atomic<std::size_t> g_chunks{0U};

// Cuts a new chunk into blocks of the class: returns the first and puts the others on `list`, in address order.
auto cut_chunk(FreeList& list, const std::size_t size_class) -> void* {
  auto* const chunk = static_cast<std::byte*>(::operator new(kChunkBytes));
  g_chunks.fetch_add(1U, std::memory_order_relaxed);
  const std::size_t size = block_bytes(size_class);
  const std::size_t count = kChunkBytes / size;
  for (std::size_t index = count - 1U; index > 0U; --index) {
    list.head = ::new (chunk + index * size) FreeBlock{list.head};
  }
  list.count += count - 1U;
  return chunk;
}

void give_all(ThreadLists& lists) noexcept {
  for (std::size_t size_class = 0U; size_class < kClassCount; ++size_class) {
    if (FreeList& list = lists.lists[size_class]; list.head != nullptr) {
      give_batch(list, size_class);
      list = {};
    }
  }
}

#if defined(_WIN32)

// MinGW emulates thread_local, at several times the cost of the Windows functions, so a thread's lists are found
// through a TLS slot. A fiber-local slot holds them too, for its callback: Windows calls it when the thread ends, and
// for every thread when the slot is freed, which happens when this library unloads or the process exits. TlsGetValue
// clears the thread's last Win32 error, so code must not make or free a value between a failing Windows call and its
// GetLastError.

// The TLS value of a thread whose lists have gone.
constexpr auto kEnded = std::uintptr_t{1U};

// The TLS slot while the lists are in use: TLS_OUT_OF_INDEXES before they are set up and after they shut down.
std::atomic<DWORD> g_lookup{TLS_OUT_OF_INDEXES};
// The TLS slot, which end_thread_lists still reads while they shut down.
DWORD g_lookup_slot = TLS_OUT_OF_INDEXES;
DWORD g_ending_slot = FLS_OUT_OF_INDEXES;
std::atomic<bool> g_shut_down{false};

void NTAPI end_thread_lists(void* data) {
  auto* const lists = static_cast<ThreadLists*>(data);
  give_all(*lists);
  // On a thread that is ending, a block freed from now on goes to the depot.
  if (TlsGetValue(g_lookup_slot) == data) {
    TlsSetValue(g_lookup_slot, reinterpret_cast<void*>(kEnded));
  }
  delete lists;
}

class ThreadSlots {
public:
  ThreadSlots() {
    g_lookup_slot = TlsAlloc();
    g_ending_slot = FlsAlloc(&end_thread_lists);
    if (g_lookup_slot != TLS_OUT_OF_INDEXES && g_ending_slot != FLS_OUT_OF_INDEXES) {
      g_lookup.store(g_lookup_slot, std::memory_order_release);
    }
  }

  ThreadSlots(const ThreadSlots&) = delete;
  auto operator=(const ThreadSlots&) -> ThreadSlots& = delete;
  ThreadSlots(ThreadSlots&&) = delete;
  auto operator=(ThreadSlots&&) -> ThreadSlots& = delete;

  // Every thread's lists go to the depot. No other thread may be allocating while this runs.
  ~ThreadSlots() {
    g_shut_down.store(true, std::memory_order_release);
    g_lookup.store(TLS_OUT_OF_INDEXES, std::memory_order_release);
    if (g_ending_slot != FLS_OUT_OF_INDEXES) {
      FlsFree(g_ending_slot);
    }
    if (g_lookup_slot != TLS_OUT_OF_INDEXES) {
      TlsFree(g_lookup_slot);
    }
  }
};

auto make_thread_lists() noexcept -> ThreadLists* {
  if (g_shut_down.load(std::memory_order_acquire)) {
    return nullptr;
  }
  static const ThreadSlots slots;
  const DWORD slot = g_lookup.load(std::memory_order_acquire);
  if (slot == TLS_OUT_OF_INDEXES || reinterpret_cast<std::uintptr_t>(TlsGetValue(slot)) == kEnded) {
    return nullptr;
  }
  auto* const lists = new (std::nothrow) ThreadLists;
  if (lists == nullptr) {
    return nullptr;
  }
  if (TlsSetValue(slot, lists) == FALSE || FlsSetValue(g_ending_slot, lists) == FALSE) {
    TlsSetValue(slot, nullptr);
    delete lists;
    return nullptr;
  }
  return lists;
}

}  // namespace

auto thread_lists() noexcept -> ThreadLists* {
  if (const DWORD slot = g_lookup.load(std::memory_order_acquire); slot != TLS_OUT_OF_INDEXES) {
    if (void* const value = TlsGetValue(slot); reinterpret_cast<std::uintptr_t>(value) > kEnded) {
      return static_cast<ThreadLists*>(value);
    }
  }
  return make_thread_lists();
}

#else

// The thread's lists for the fast path; t_end owns them.
thread_local ThreadLists* t_lists = nullptr;
thread_local bool t_ended = false;

// Owns the thread's lists, and gives them to the depot when the thread ends.
struct ThreadEnd {
  ThreadEnd() = default;
  ThreadEnd(const ThreadEnd&) = delete;
  auto operator=(const ThreadEnd&) -> ThreadEnd& = delete;
  ThreadEnd(ThreadEnd&&) = delete;
  auto operator=(ThreadEnd&&) -> ThreadEnd& = delete;

  ~ThreadEnd() {
    t_ended = true;
    t_lists = nullptr;
    if (lists != nullptr) {
      give_all(*lists);
      delete lists;
    }
  }

  ThreadLists* lists = nullptr;
};

thread_local ThreadEnd t_end;

auto make_thread_lists() noexcept -> ThreadLists* {
  if (t_ended) {
    return nullptr;
  }
  auto* const lists = new (std::nothrow) ThreadLists;
  if (lists == nullptr) {
    return nullptr;
  }
  // Using the guard constructs it, and its destructor runs when the thread ends.
  t_end.lists = lists;
  t_lists = lists;
  return lists;
}

}  // namespace

auto thread_lists() noexcept -> ThreadLists* {
  if (t_lists != nullptr) {
    return t_lists;
  }
  return make_thread_lists();
}

#endif

auto refill(FreeList& list, const std::size_t size_class) -> void* {
  list = take_batch(size_class);
  if (list.head == nullptr) {
    return cut_chunk(list, size_class);
  }
  FreeBlock* const head = list.head;
  list.head = head->next;
  --list.count;
  return head;
}

void spill(FreeList& list, const std::size_t size_class) noexcept {
  // The thread keeps the blocks it freed last, which are the likeliest to be in its cache.
  const std::size_t keep = list.count / 2U;
  FreeBlock* last_kept = list.head;
  for (std::size_t index = 1U; index < keep; ++index) {
    last_kept = last_kept->next;
  }
  const FreeList batch{.head = last_kept->next, .count = list.count - keep};
  last_kept->next = nullptr;
  list.count = keep;
  give_batch(batch, size_class);
}

auto take_shared(const std::size_t size_class) -> void* {
  {
    Shelf& shelf = depot().shelves[size_class];
    const std::scoped_lock lock(shelf.mutex);
    if (shelf.batch_count != 0U) {
      FreeList& batch = shelf.batches[shelf.batch_count - 1U];
      FreeBlock* const head = batch.head;
      batch.head = head->next;
      if (--batch.count == 0U) {
        --shelf.batch_count;
      }
      return head;
    }
  }
  FreeList rest;
  void* const block = cut_chunk(rest, size_class);
  give_batch(rest, size_class);
  return block;
}

void give_shared(void* block, const std::size_t size_class) noexcept {
  give_batch(FreeList{.head = ::new (block) FreeBlock{nullptr}, .count = 1U}, size_class);
}

void report_bad_free(const void* block, const std::size_t bytes) noexcept {
  std::fprintf(stderr, "fleaux heap: block %p freed with %zu bytes is not a live block of that size\n", block, bytes);
  std::abort();
}

auto thread_list_count(const std::size_t size_class) noexcept -> std::size_t {
  const ThreadLists* const lists = thread_lists();
  return lists == nullptr ? 0U : lists->lists[size_class].count;
}

auto depot_count(const std::size_t size_class) noexcept -> std::size_t {
  Shelf& shelf = depot().shelves[size_class];
  const std::scoped_lock lock(shelf.mutex);
  std::size_t count = 0U;
  for (std::size_t index = 0U; index < shelf.batch_count; ++index) {
    count += shelf.batches[index].count;
  }
  return count;
}

auto chunk_count() noexcept -> std::size_t { return g_chunks.load(std::memory_order_relaxed); }

}  // namespace fleaux::runtime::heap::detail
