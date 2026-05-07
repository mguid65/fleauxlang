#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/common/indirect_optional.hpp"

namespace {

struct RecursiveNode;
using RecursiveNodeOptional = fleaux::common::IndirectOptional<RecursiveNode>;

struct TrulyIncomplete;

struct RecursiveNode {
  int value = 0;
  RecursiveNodeOptional next;
};

struct Payload {
  std::vector<int> values;

  auto operator==(const Payload&) const -> bool = default;
};

struct CopyOnly {
  int value = 0;

  CopyOnly() = default;
  explicit CopyOnly(int value) : value(value) {}

  CopyOnly(const CopyOnly&) = default;
  CopyOnly(CopyOnly&&) = delete;
  auto operator=(const CopyOnly&) -> CopyOnly& = default;
  auto operator=(CopyOnly&&) -> CopyOnly& = delete;
};

struct MoveOnly {
  int value = 0;

  MoveOnly() = default;
  explicit MoveOnly(int value) : value(value) {}

  MoveOnly(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
  auto operator=(const MoveOnly&) -> MoveOnly& = delete;
  auto operator=(MoveOnly&&) -> MoveOnly& = default;
};

struct NotConvertible {};

}  // namespace

template <class T, class U>
concept HasConstValueOr = requires(const fleaux::common::IndirectOptional<T>& maybe, U&& fallback) {
  maybe.value_or(std::forward<U>(fallback));
};

template <class T, class U>
concept HasRvalueValueOr = requires(fleaux::common::IndirectOptional<T>&& maybe, U&& fallback) {
  std::move(maybe).value_or(std::forward<U>(fallback));
};

template <class T, class... Args>
concept HasMakeIndirectOptional = requires(Args&&... args) {
  fleaux::common::make_indirect_optional<T>(std::forward<Args>(args)...);
};

template <class T, class U, class... Args>
concept HasMakeIndirectOptionalInitList = requires(std::initializer_list<U> init, Args&&... args) {
  fleaux::common::make_indirect_optional<T>(init, std::forward<Args>(args)...);
};

template <class T, class... Args>
concept HasInPlaceConstruction = requires(Args&&... args) {
  fleaux::common::IndirectOptional<T>{std::in_place, std::forward<Args>(args)...};
};

template <class T, class U, class... Args>
concept HasInPlaceConstructionInitList = requires(std::initializer_list<U> init, Args&&... args) {
  fleaux::common::IndirectOptional<T>{std::in_place, init, std::forward<Args>(args)...};
};

template <class T, class... Args>
concept HasEmplace = requires(fleaux::common::IndirectOptional<T>& maybe, Args&&... args) {
  maybe.emplace(std::forward<Args>(args)...);
};

template <class T, class U, class... Args>
concept HasEmplaceInitList = requires(fleaux::common::IndirectOptional<T>& maybe, std::initializer_list<U> init, Args&&... args) {
  maybe.emplace(init, std::forward<Args>(args)...);
};

template <class T, class U>
concept HasDirectValueConstruction = requires(U&& value) {
  fleaux::common::IndirectOptional<T>{std::forward<U>(value)};
};

template <class T, class U>
concept HasDirectValueAssignment = requires(fleaux::common::IndirectOptional<T>& maybe, U&& value) {
  maybe = std::forward<U>(value);
};

static_assert(HasConstValueOr<int, int>);
static_assert(!HasConstValueOr<CopyOnly, const CopyOnly&>);
static_assert(!HasConstValueOr<MoveOnly, const MoveOnly&>);
static_assert(HasRvalueValueOr<MoveOnly, MoveOnly>);
static_assert(!HasConstValueOr<int, NotConvertible>);
static_assert(HasMakeIndirectOptional<int, int>);
static_assert(HasMakeIndirectOptional<std::vector<int>, std::size_t, int>);
static_assert(HasMakeIndirectOptionalInitList<std::vector<int>, int>);
static_assert(!HasMakeIndirectOptional<int, NotConvertible>);
static_assert(HasInPlaceConstruction<int, int>);
static_assert(HasInPlaceConstruction<std::vector<int>, std::size_t, int>);
static_assert(HasInPlaceConstructionInitList<std::vector<int>, int>);
static_assert(!HasInPlaceConstruction<int, NotConvertible>);
static_assert(!HasInPlaceConstruction<TrulyIncomplete, int>);
static_assert(HasEmplace<int, int>);
static_assert(HasEmplace<std::vector<int>, std::size_t, int>);
static_assert(HasEmplaceInitList<std::vector<int>, int>);
static_assert(!HasEmplace<int, NotConvertible>);
static_assert(!HasEmplace<TrulyIncomplete, int>);
static_assert(HasDirectValueConstruction<int, int>);
static_assert(HasDirectValueConstruction<CopyOnly, const CopyOnly&>);
static_assert(!HasDirectValueConstruction<CopyOnly, CopyOnly>);
static_assert(HasDirectValueConstruction<MoveOnly, MoveOnly>);
static_assert(!HasDirectValueConstruction<MoveOnly, const MoveOnly&>);
static_assert(!HasDirectValueConstruction<int, NotConvertible>);
static_assert(!HasDirectValueConstruction<TrulyIncomplete, int>);
static_assert(HasDirectValueAssignment<int, int>);
static_assert(HasDirectValueAssignment<CopyOnly, const CopyOnly&>);
static_assert(!HasDirectValueAssignment<CopyOnly, CopyOnly>);
static_assert(HasDirectValueAssignment<MoveOnly, MoveOnly>);
static_assert(!HasDirectValueAssignment<MoveOnly, const MoveOnly&>);
static_assert(!HasDirectValueAssignment<int, NotConvertible>);
static_assert(!HasDirectValueAssignment<TrulyIncomplete, int>);

TEST_CASE("IndirectOptional behaves like a deep-copying optional holder", "[common][indirect_optional]") {
  using fleaux::common::IndirectOptional;
  using fleaux::common::make_indirect_optional;

  SECTION("default construction and nullopt are disengaged") {
    IndirectOptional<int> maybe_int;
    IndirectOptional<int> maybe_nullopt{std::nullopt};

    REQUIRE_FALSE(maybe_int.has_value());
    REQUIRE_FALSE(static_cast<bool>(maybe_int));
    REQUIRE(maybe_int == std::nullopt);
    REQUIRE(maybe_nullopt == std::nullopt);
    REQUIRE_THROWS_AS(maybe_int.value(), std::bad_optional_access);

    maybe_int.emplace(42);
    REQUIRE(maybe_int.has_value());
    REQUIRE(*maybe_int == 42);

    maybe_int = std::nullopt;
    REQUIRE_FALSE(maybe_int.has_value());
  }

  SECTION("in_place construction assignment and value_or mirror optional habits") {
    IndirectOptional<std::vector<int>> values{std::in_place, 3U, 7};
    REQUIRE(values.has_value());
    REQUIRE(values->size() == 3U);
    REQUIRE((*values)[1] == 7);

    values = std::vector<int>{1, 2, 3};
    REQUIRE(values.value() == std::vector<int>{1, 2, 3});
    REQUIRE(values.value_or(std::vector<int>{9, 9}) == std::vector<int>{1, 2, 3});

    values.reset();
    REQUIRE(values.value_or(std::vector<int>{9, 9}) == std::vector<int>{9, 9});

    IndirectOptional<std::vector<int>> moved_values{std::in_place, std::vector<int>{4, 5, 6}};
    REQUIRE(std::move(moved_values).value_or(std::vector<int>{1, 1}) == std::vector<int>{4, 5, 6});
  }

  SECTION("copying preserves value semantics") {
    auto original = make_indirect_optional<Payload>(Payload{.values = {1, 2, 3}});
    auto copy = original;

    copy->values[1] = 99;

    REQUIRE(original->values == std::vector<int>{1, 2, 3});
    REQUIRE(copy->values == std::vector<int>{1, 99, 3});
  }

  SECTION("make_indirect_optional participates only for valid constructions") {
    auto values = make_indirect_optional<std::vector<int>>({7, 8, 9});

    REQUIRE(values.has_value());
    REQUIRE(*values == std::vector<int>{7, 8, 9});
  }

  SECTION("in_place construction and emplace participate only for valid constructions") {
    IndirectOptional<std::vector<int>> values{std::in_place, 2U, 5};

    REQUIRE(values.has_value());
    REQUIRE(*values == std::vector<int>{5, 5});

    auto& emplaced = values.emplace({1, 2, 3});

    REQUIRE(&emplaced == &values.value());
    REQUIRE(values.value() == std::vector<int>{1, 2, 3});
  }

  SECTION("direct value construction and assignment preserve value-category participation") {
    CopyOnly copy_source{11};
    IndirectOptional<CopyOnly> copied{copy_source};

    REQUIRE(copied.has_value());
    REQUIRE(copied->value == 11);

    MoveOnly move_source{23};
    IndirectOptional<MoveOnly> moved{std::move(move_source)};

    REQUIRE(moved.has_value());
    REQUIRE(moved->value == 23);

    copied = copy_source;
    REQUIRE(copied->value == 11);

    moved = MoveOnly{29};
    REQUIRE(moved->value == 29);
  }

  SECTION("move construction transfers ownership and disengages the source") {
    IndirectOptional<Payload> original{std::in_place, Payload{.values = {4, 5, 6}}};
    IndirectOptional<Payload> moved{std::move(original)};

    REQUIRE(moved.has_value());
    REQUIRE_FALSE(original.has_value());
    REQUIRE(moved->values == std::vector<int>{4, 5, 6});
  }

  SECTION("swap and equality compare like optionals") {
    IndirectOptional<int> lhs{1};
    IndirectOptional<int> rhs{std::nullopt};

    swap(lhs, rhs);
    REQUIRE(lhs == std::nullopt);
    REQUIRE(rhs == 1);
    REQUIRE(rhs != 2);

    IndirectOptional<int> rhs_copy{1};
    REQUIRE(rhs == rhs_copy);
  }

  SECTION("ordered comparisons treat disengaged values like optional") {
    IndirectOptional<int> empty;
    IndirectOptional<int> one{1};
    IndirectOptional<int> two{2};

    REQUIRE(empty < one);
    REQUIRE(std::nullopt < one);
    REQUIRE(one > std::nullopt);
    REQUIRE(one < two);
    REQUIRE(two > one);
    REQUIRE(one <= 1);
    REQUIRE(one >= 1);
    REQUIRE(one < 2);
    REQUIRE(0 < one);
    REQUIRE_FALSE(one < std::nullopt);
    REQUIRE_FALSE(two < one);
  }


  SECTION("recursive incomplete-style holders remain usable") {
    RecursiveNodeOptional root;
    root.emplace(RecursiveNode{.value = 1});
    root->next.emplace(RecursiveNode{.value = 2});

    auto copy = root;
    copy->next->value = 7;

    REQUIRE(root->value == 1);
    REQUIRE(root->next.has_value());
    REQUIRE(root->next->value == 2);
    REQUIRE(copy->next->value == 7);
  }
}


