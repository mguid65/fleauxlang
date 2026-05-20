#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "fleaux/common/box.hpp"

namespace {

struct Payload {
  std::vector<int> values{};

  auto operator==(const Payload&) const -> bool = default;
};

}  // namespace

TEST_CASE("Box supports deep-copying ownership from the common header", "[common][box]") {
  fleaux::common::Box<Payload> original(Payload{.values = {1, 2, 3}});
  fleaux::common::Box<Payload> copied = original;

  REQUIRE(copied->values == std::vector<int>{1, 2, 3});

  original->values.push_back(4);
  REQUIRE(original->values == std::vector<int>{1, 2, 3, 4});
  REQUIRE(copied->values == std::vector<int>{1, 2, 3});

  copied.emplace(Payload{.values = {9}});
  REQUIRE(copied->values == std::vector<int>{9});
  REQUIRE(original->values == std::vector<int>{1, 2, 3, 4});
}



