#include <memory>
#include <tuple>
#include <utility>
#include <variant>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/utility.hpp"

namespace {

struct Point {
  int x;
  int y;
};

template <std::size_t I>
auto get(Point& point) -> int& {
  static_assert(I < 2);
  if constexpr (I == 0) {
    return point.x;
  } else {
    return point.y;
  }
}

template <std::size_t I>
auto get(const Point& point) -> const int& {
  static_assert(I < 2);
  if constexpr (I == 0) {
    return point.x;
  } else {
    return point.y;
  }
}

template <std::size_t I>
auto get(Point&& point) -> int&& {
  static_assert(I < 2);
  if constexpr (I == 0) {
    return std::move(point.x);
  } else {
    return std::move(point.y);
  }
}

template <std::size_t I>
auto get(const Point&& point) -> const int&& {
  static_assert(I < 2);
  if constexpr (I == 0) {
    return std::move(point.x);
  } else {
    return std::move(point.y);
  }
}

}  // namespace

namespace std {

template <>
struct tuple_size<Point> : integral_constant<std::size_t, 2> {};

template <>
struct tuple_element<0, Point> {
  using type = int;
};

template <>
struct tuple_element<1, Point> {
  using type = int;
};

}  // namespace std

TEST_CASE("structured_visit prefers the original whole-object signature when both forms are available",
          "[common][utility][structured_visit]") {
  const std::variant<Point> point{Point{3, 4}};

  const auto result =
      fleaux::utility::structured_visit<int>(fleaux::common::overloaded{
                                                 [](const Point& value) { return value.x * 10 + value.y; },
                                                 [](int, int) { return -1; },
                                             },
                                             point);

  REQUIRE(result == 34);
}

TEST_CASE("structured_visit falls back to unpacked tuple-like elements when needed",
          "[common][utility][structured_visit]") {
  constexpr std::variant<std::pair<int, int>> value{std::pair{2, 5}};
  constexpr auto result =
      fleaux::utility::structured_visit<int>([](const int lhs, const int rhs) { return lhs + rhs; }, value);

  REQUIRE(result == 7);
}

TEST_CASE("structured_visit can mix whole values and unpacked tuple-like values",
          "[common][utility][structured_visit]") {
  const std::variant<std::pair<int, int>> lhs{std::pair{2, 3}};
  const std::variant<Point> rhs{Point{5, 7}};

  const auto result = fleaux::utility::structured_visit<int>(
      [](const std::pair<int, int>& pair_value, int x, int y) { return pair_value.first + pair_value.second + x + y; },
      lhs, rhs);

  REQUIRE(result == 17);
}

TEST_CASE("structured_visit can unpack multiple tuple-like inputs", "[common][utility][structured_visit]") {
  const std::variant<std::pair<int, int>> lhs{std::pair{1, 2}};
  const std::variant<std::pair<int, int>> rhs{std::pair{3, 4}};
  const auto result =
      fleaux::utility::structured_visit<int>([](int a, int b, int c, int d) { return a + b + c + d; }, lhs, rhs);

  REQUIRE(result == 10);
}

TEST_CASE("structured_visit handles every alternative of a multi-alternative variant with overloaded visitors",
          "[common][utility][structured_visit]") {
  using Value = std::variant<char, std::tuple<int, int, int>, float>;

  const auto visitor = fleaux::common::overloaded{
      [](char) { return 101; },
      [](int a, int b, int c) { return a * b + c; },
      [](const std::tuple<int, int, int>& value) { return std::get<0>(value) * std::get<1>(value) + std::get<2>(value); },
      [](float) { return 103; },
  };

  REQUIRE(fleaux::utility::structured_visit<int>(visitor, Value{'x'}) == 101);
  REQUIRE(fleaux::utility::structured_visit<int>(visitor, Value{std::in_place_type<std::tuple<int, int, int>>, 2, 3, 5}) == 11);
  REQUIRE(fleaux::utility::structured_visit<int>(visitor, Value{1.25F}) == 103);
}

TEST_CASE("structured_visit can handle every combination from multiple multi-alternative variants",
          "[common][utility][structured_visit]") {
  using LhsVariant = std::variant<bool, std::tuple<int, int, int>, Point>;
  using RhsVariant = std::variant<std::monostate, std::tuple<int, int, int>, long>;

  const auto visitor = fleaux::common::overloaded{
      [](bool, std::monostate) { return 101; },
      [](bool, int, int, int) { return 102; },
      [](bool, const std::tuple<int, int, int>& value) {
        return 100 + std::get<0>(value) - 2;
      },
      [](bool, long) { return 103; },
      [](int, int, int, std::monostate) { return 104; },
      [](int, int, int, int, int, int) { return 105; },
      [](const std::tuple<int, int, int>&, const std::tuple<int, int, int>&) { return 105; },
      [](const std::tuple<int, int, int>&, std::monostate) { return 104; },
      [](int, int, int, long) { return 106; },
      [](const std::tuple<int, int, int>&, long) { return 106; },
      [](const Point&, std::monostate) { return 107; },
      [](const Point&, int, int, int) { return 108; },
      [](const Point&, const std::tuple<int, int, int>&) { return 108; },
      [](const Point&, long) { return 109; },
  };

  const auto check = [&](LhsVariant lhs, RhsVariant rhs, int expected) {
    REQUIRE(fleaux::utility::structured_visit<int>(visitor, lhs, rhs) == expected);
  };

  check(LhsVariant{true}, RhsVariant{std::monostate{}}, 101);
  check(LhsVariant{true}, RhsVariant{std::in_place_type<std::tuple<int, int, int>>, 4, 5, 6}, 102);
  check(LhsVariant{true}, RhsVariant{9L}, 103);
  check(LhsVariant{std::in_place_type<std::tuple<int, int, int>>, 1, 2, 3}, RhsVariant{std::monostate{}}, 104);
  check(LhsVariant{std::in_place_type<std::tuple<int, int, int>>, 1, 2, 3},
        RhsVariant{std::in_place_type<std::tuple<int, int, int>>, 4, 5, 6}, 105);
  check(LhsVariant{std::in_place_type<std::tuple<int, int, int>>, 1, 2, 3}, RhsVariant{9L}, 106);
  check(LhsVariant{Point{7, 8}}, RhsVariant{std::monostate{}}, 107);
  check(LhsVariant{Point{7, 8}}, RhsVariant{std::in_place_type<std::tuple<int, int, int>>, 4, 5, 6}, 108);
  check(LhsVariant{Point{7, 8}}, RhsVariant{9L}, 109);
}

TEST_CASE("structured_visit preserves lvalue element references when unpacking",
          "[common][utility][structured_visit]") {
  std::variant<std::pair<int, int>> value{std::pair{1, 2}};

  fleaux::utility::structured_visit<void>(
      [](int& lhs, int& rhs) {
        lhs += 10;
        rhs += 20;
      },
      value);

  REQUIRE(std::get<std::pair<int, int>>(value).first == 11);
  REQUIRE(std::get<std::pair<int, int>>(value).second == 22);
}

TEST_CASE("structured_visit preserves Point lvalue element references when unpacking",
          "[common][utility][structured_visit]") {
  std::variant<Point> value{Point{1, 2}};

  fleaux::utility::structured_visit<void>(
      [](int& x, int& y) {
        x += 10;
        y += 20;
      },
      value);

  const auto& point = std::get<Point>(value);
  REQUIRE(point.x == 11);
  REQUIRE(point.y == 22);
}

TEST_CASE("structured_visit binds const Point lvalue elements when unpacking",
          "[common][utility][structured_visit]") {
  const std::variant<Point> value{Point{3, 5}};

  const auto result = fleaux::utility::structured_visit<int>(
      [](const int& x, const int& y) {
        return x * 10 + y;
      },
      value);

  REQUIRE(result == 35);
}

TEST_CASE("structured_visit forwards Point rvalue elements when unpacking",
          "[common][utility][structured_visit]") {
  std::variant<Point> value{Point{4, 6}};

  const auto result = fleaux::utility::structured_visit<int>(
      fleaux::common::overloaded{
          [](int&& x, int&& y) { return x * 10 + y; },
          [](const int&, const int&) { return -1; },
      },
      std::move(value));

  REQUIRE(result == 46);
}

TEST_CASE("structured_visit forwards const Point rvalue elements when unpacking",
          "[common][utility][structured_visit]") {
  const std::variant<Point> value{Point{7, 9}};

  const auto result = fleaux::utility::structured_visit<int>(
      fleaux::common::overloaded{
          [](const int&& x, const int&& y) { return x * 10 + y; },
          [](const int&, const int&) { return -1; },
      },
      std::move(value));

  REQUIRE(result == 79);
}

TEST_CASE("structured_visit forwards rvalue tuple-like elements", "[common][utility][structured_visit]") {
  std::variant<std::tuple<std::unique_ptr<int>>> value{std::in_place_type<std::tuple<std::unique_ptr<int>>>,
                                                       std::make_unique<int>(42)};
  const auto result =
      fleaux::utility::structured_visit<int>([](std::unique_ptr<int> ptr) { return *ptr; }, std::move(value));

  REQUIRE(result == 42);
}

TEST_CASE("structured_visit supports empty tuple-like unpacking", "[common][utility][structured_visit]") {
  const std::variant<std::tuple<>> value{std::tuple<>{}};
  const auto result = fleaux::utility::structured_visit<int>([]() { return 7; }, value);

  REQUIRE(result == 7);
}

TEST_CASE("structured_visit falls back to recursive dispatch for larger alternative sets",
          "[common][utility][structured_visit]") {
  const std::variant<int> a{1};
  const std::variant<int> b{2};
  const std::variant<int> c{3};
  const std::variant<int> d{4};
  using TailVariant = std::variant<std::monostate, std::tuple<int, int, int>, std::pair<int, int>>;

  const auto visitor = fleaux::common::overloaded{
      [](int w, int x, int y, int z, std::monostate) { return w + x + y + z; },
      [](int w, int x, int y, int z, int lhs, int mid, int rhs) { return w + x + y + z + lhs + mid + rhs; },
      [](int w, int x, int y, int z, const std::tuple<int, int, int>& tail) {
        return w + x + y + z + std::get<0>(tail) + std::get<1>(tail) + std::get<2>(tail);
      },
      [](int w, int x, int y, int z, const std::pair<int, int>& tail) { return w + x + y + z + tail.first + tail.second; },
  };

  REQUIRE(fleaux::utility::structured_visit<int>(visitor, a, b, c, d, TailVariant{std::monostate{}}) == 10);
  REQUIRE(fleaux::utility::structured_visit<int>(
              visitor, a, b, c, d, TailVariant{std::in_place_type<std::tuple<int, int, int>>, 5, 6, 7}) == 28);
  REQUIRE(fleaux::utility::structured_visit<int>(visitor, a, b, c, d, TailVariant{std::pair{8, 9}}) == 27);
}
