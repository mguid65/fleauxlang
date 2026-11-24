#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/heap.hpp"
#include "fleaux/runtime/runtime_support.hpp"

namespace heap = fleaux::runtime::heap;
using fleaux::runtime::Value;

namespace {

// The largest request whose block comes from `size_class`.
constexpr auto largest_request(const std::size_t size_class) -> std::size_t {
  return heap::detail::block_bytes(size_class) - heap::kCheckBytes;
}

// The blocks in each of the calling thread's lists.
auto thread_list_counts() -> std::array<std::size_t, heap::kClassCount> {
  std::array<std::size_t, heap::kClassCount> counts{};
  for (std::size_t size_class = 0U; size_class < heap::kClassCount; ++size_class) {
    counts[size_class] = heap::detail::thread_list_count(size_class);
  }
  return counts;
}

}  // namespace

TEST_CASE("Heap blocks are aligned, distinct, and sized by their class", "[heap]") {
  SECTION("Every request size up to past the largest class") {
    for (std::size_t bytes = 1U; bytes <= heap::kMaxBlockBytes + 64U; ++bytes) {
      void* const first = heap::allocate(bytes);
      void* const second = heap::allocate(bytes);
      CHECK(first != second);
      CHECK(reinterpret_cast<std::uintptr_t>(first) % heap::kAlignment == 0U);
      CHECK(reinterpret_cast<std::uintptr_t>(second) % heap::kAlignment == 0U);
      std::memset(first, 0xA5, bytes);
      std::memset(second, 0x5A, bytes);
      CHECK(heap::detail::is_live_block(first, bytes));
      heap::deallocate(second, bytes);
      heap::deallocate(first, bytes);
    }
  }

  SECTION("A class holds the requests between its neighbours' blocks") {
    CHECK(heap::size_class_of(0U) == heap::kClassCount);
    CHECK(heap::size_class_of(heap::kMaxBlockBytes - heap::kCheckBytes + 1U) == heap::kClassCount);
    for (std::size_t size_class = 0U; size_class < heap::kClassCount; ++size_class) {
      if (largest_request(size_class) == 0U) {
        continue;  // In Debug builds the check fills the smallest class's blocks.
      }
      CHECK(heap::size_class_of(largest_request(size_class)) == size_class);
      if (size_class + 1U < heap::kClassCount) {
        CHECK(heap::size_class_of(largest_request(size_class) + 1U) == size_class + 1U);
      }
    }
  }
}

TEST_CASE("A thread reuses the block it freed last", "[heap]") {
  constexpr std::size_t bytes = 48U;
  constexpr std::size_t same_class = 40U;
  REQUIRE(heap::size_class_of(same_class) == heap::size_class_of(bytes));

  void* const first = heap::allocate(bytes);
  heap::deallocate(first, bytes);
  void* const again = heap::allocate(same_class);
  CHECK(again == first);

  void* const other_class = heap::allocate(4U * bytes);
  CHECK(other_class != first);
  heap::deallocate(other_class, 4U * bytes);
  heap::deallocate(again, same_class);
}

TEST_CASE("A block freed on another thread is reused there", "[heap]") {
  constexpr std::size_t bytes = 48U;
  void* const block = heap::allocate(bytes);
  void* reused = nullptr;
  std::thread([&] {
    heap::deallocate(block, bytes);
    reused = heap::allocate(bytes);
    heap::deallocate(reused, bytes);
  }).join();
  CHECK(reused == block);
}

TEST_CASE("Blocks a thread does not keep go to the depot, and other threads take them", "[heap]") {
  constexpr std::size_t size_class = heap::kClassCount - 1U;
  const std::size_t bytes = largest_request(size_class);
  const std::size_t limit = heap::detail::kListLimits[size_class];

  bool stayed_within_limit = true;
  bool depot_gained = false;
  std::size_t list_at_end = 0U;
  std::size_t depot_at_end = 0U;
  std::thread([&] {
    std::vector<void*> blocks(3U * limit);
    for (void*& block : blocks) {
      block = heap::allocate(bytes);
    }
    const std::size_t depot_before = heap::detail::depot_count(size_class);
    for (void* const block : blocks) {
      heap::deallocate(block, bytes);
      stayed_within_limit = stayed_within_limit && heap::detail::thread_list_count(size_class) <= limit;
    }
    depot_gained = heap::detail::depot_count(size_class) > depot_before;
    list_at_end = heap::detail::thread_list_count(size_class);
    depot_at_end = heap::detail::depot_count(size_class);
  }).join();

  CHECK(stayed_within_limit);
  CHECK(depot_gained);
  // The thread's lists went to the depot when it ended.
  CHECK(list_at_end > 0U);
  CHECK(heap::detail::depot_count(size_class) == depot_at_end + list_at_end);

  // A new thread takes a batch from the depot instead of cutting a chunk, and gives it back when it ends.
  const std::size_t chunks_before = heap::detail::chunk_count();
  const std::size_t depot_before = heap::detail::depot_count(size_class);
  std::thread([&] {
    void* const block = heap::allocate(bytes);
    heap::deallocate(block, bytes);
  }).join();
  CHECK(heap::detail::chunk_count() == chunks_before);
  CHECK(heap::detail::depot_count(size_class) == depot_before);
}

TEST_CASE("Debug builds know each block's size and whether it is live", "[heap]") {
  constexpr std::size_t bytes = 48U;
  void* const block = heap::allocate(bytes);
  CHECK(heap::detail::is_live_block(block, bytes));
  if constexpr (heap::kCheckBytes != 0U) {
    CHECK_FALSE(heap::detail::is_live_block(block, bytes + 16U));
    heap::deallocate(block, bytes);
    CHECK_FALSE(heap::detail::is_live_block(block, bytes));
  } else {
    heap::deallocate(block, bytes);
  }
}

TEST_CASE("Every kind of heap object comes from the allocator and goes back to it", "[heap][runtime]") {
  using namespace fleaux::runtime;

  SECTION("A tuple's block") {
    TupleBuilder items(2U);
    items.push(make_int(1));
    items.push(make_int(2));
    Value tuple = std::move(items).finish();
    const void* const block = reinterpret_cast<const std::byte*>(tuple.tuple().data()) - kTupleItemsOffset;
    tuple = Value{};
    void* const next = heap::allocate(tuple_block_bytes(2U));
    CHECK(next == block);
    heap::deallocate(next, tuple_block_bytes(2U));
  }

  SECTION("Tuples that grow, shrink, and are copied before a change") {
    TupleBuilder growing(1U);
    for (Int index = 0; index < 40; ++index) {
      growing.push(make_int(index));
    }
    Value tuple = std::move(growing).finish();
    REQUIRE(tuple.tuple().size() == 40U);

    TupleBuilder reopened = TupleBuilder::reopen(std::move(tuple), 64U);
    reopened.erase(0U, 38U);
    Value shrunk = std::move(reopened).finish();
    REQUIRE(shrunk.tuple().size() == 2U);

    const Value shared = shrunk;
    Value changed = shrunk;
    changed.mutable_tuple()[0] = make_int(7);
    CHECK(shared.tuple()[0] == make_int(38));
    CHECK(changed.tuple()[0] == make_int(7));
  }

  SECTION("A string, a dict with its entries, a function value, and a task") {
    Value string = make_string("short");
    Dict entries;
    for (Int key = 0; key < 3; ++key) {
      entries.set(make_int(key), make_int(key * 10));
    }
    Value dict = make_dict(std::move(entries));
    Value function = make_function([](const Value& value) -> Value { return value; });

    // Destroying each one gives its blocks back to the thread's lists.
    auto expected = thread_list_counts();
    string = Value{};
    ++expected[heap::size_class_of(sizeof(StringObject))];
    CHECK(thread_list_counts() == expected);

    dict = Value{};
    ++expected[heap::size_class_of(sizeof(DictObject))];
    ++expected[heap::size_class_of(4U * sizeof(Dict::Entry))];
    CHECK(thread_list_counts() == expected);

    function = Value{};
    const auto after = thread_list_counts();
    std::size_t returned = 0U;
    for (std::size_t size_class = 0U; size_class < heap::kClassCount; ++size_class) {
      returned += after[size_class] - expected[size_class];
    }
    CHECK(returned == 1U);

    Value task = make_task();
    task = Value{};
  }

  SECTION("A dict that grows past its linear entries and shrinks back") {
    Dict dict;
    for (Int key = 0; key < 20; ++key) {
      dict.set(make_int(key), make_string("value " + std::to_string(key)));
    }
    Value value = make_dict(std::move(dict));
    const Value shared = value;
    for (Int key = 0; key < 16; ++key) {
      CHECK(value.mutable_dict().erase(make_int(key)));
    }
    CHECK(value.dict().size() == 4U);
    CHECK(shared.dict().size() == 20U);
    const Value* found = shared.dict().find(make_int(19));
    REQUIRE(found != nullptr);
    CHECK(as_string(*found) == "value 19");
  }
}

TEST_CASE("Values made on worker threads and freed on the main thread stay within the lists' limits",
          "[heap][parallel]") {
  const std::string source =
      "import Std;\n"
      "let Pair(n: Int64): Tuple<Int64, Int64> = (n, (n, 1) -> Std.Add);\n"
      "((256) -> Std.Tuple.Range, Pair) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Length -> Std.Println;\n";
  std::string captured;
  fleaux::embed::VmHostConfig config;
  config.stdout_sink = [&captured](const std::string_view text) { captured.append(text); };
  fleaux::embed::VmHost host(config);

  for (int run = 0; run < 40; ++run) {
    const auto result = host.run_source("heap_parallel.fleaux", source);
    REQUIRE(result.has_value());
    for (std::size_t size_class = 0U; size_class < heap::kClassCount; ++size_class) {
      CHECK(heap::detail::thread_list_count(size_class) <= heap::detail::kListLimits[size_class]);
    }
  }
  CHECK(captured.size() == 40U * std::string{"256\n"}.size());
}
