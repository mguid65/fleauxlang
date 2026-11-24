#include "fleaux/runtime/heap.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>

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

// A thread's lists go to the depot when the thread ends, and when this library unloads or the process exits.
void end_thread_lists(void* value) noexcept {
  auto* const lists = static_cast<ThreadLists*>(value);
  give_all(*lists);
  delete lists;
}

}  // namespace

constinit common::ThreadSlot g_lists_slot{&end_thread_lists};

auto make_thread_lists() noexcept -> ThreadLists* {
  auto* const lists = new (std::nothrow) ThreadLists;
  if (lists == nullptr) {
    return nullptr;
  }
  if (!g_lists_slot.set(lists)) {
    delete lists;
    return nullptr;
  }
  return lists;
}

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
