#pragma once

#include <cassert>
#include <concepts>
#include <initializer_list>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace fleaux::common {

template <class T>
class IndirectOptional;

namespace detail {

template <class T>
concept complete_type = requires { sizeof(T); };

template <class T, class U>
concept indirect_optional_value_or_lvalue = complete_type<T> && std::copy_constructible<T> && std::constructible_from<T, U&&>;

template <class T, class U>
concept indirect_optional_value_or_rvalue = complete_type<T> && std::move_constructible<T> && std::constructible_from<T, U&&>;

template <class T, class U>
concept indirect_optional_direct_value_source = std::same_as<std::remove_cvref_t<U>, T>;

template <class T, class U>
concept indirect_optional_direct_value_lvalue_construction = complete_type<T> &&
                                                             indirect_optional_direct_value_source<T, U> &&
                                                             std::is_lvalue_reference_v<U&&> &&
                                                             std::constructible_from<T, U&&>;

template <class T, class U>
concept indirect_optional_direct_value_rvalue_construction = complete_type<T> &&
                                                             indirect_optional_direct_value_source<T, U> &&
                                                             !std::is_lvalue_reference_v<U&&> &&
                                                             std::constructible_from<T, U&&>;

template <class T, class U>
concept indirect_optional_direct_value_lvalue_assignment = indirect_optional_direct_value_lvalue_construction<T, U> &&
                                                          std::assignable_from<T&, U&&>;

template <class T, class U>
concept indirect_optional_direct_value_rvalue_assignment = indirect_optional_direct_value_rvalue_construction<T, U> &&
                                                          std::assignable_from<T&, U&&>;

template <class T, class... Args>
concept indirect_optional_factory = complete_type<T> && std::constructible_from<T, Args&&...>;

template <class T, class U, class... Args>
concept indirect_optional_initializer_list_factory = complete_type<T> &&
                                                    std::constructible_from<T, std::initializer_list<U>, Args&&...>;

}  // namespace detail

template <class T>
class IndirectOptional {
public:
  using value_type = T;

  constexpr IndirectOptional() noexcept = default;
  explicit constexpr IndirectOptional(std::nullopt_t) noexcept {}

  template <class U>
    requires detail::indirect_optional_direct_value_lvalue_construction<T, U>
  explicit IndirectOptional(U&& value) {
    (void)construct(std::forward<U>(value));
  }

  template <class U>
    requires detail::indirect_optional_direct_value_rvalue_construction<T, U>
  explicit IndirectOptional(U&& value) {
    (void)construct(std::forward<U>(value));
  }

  template <class... Args>
    requires detail::indirect_optional_factory<T, Args...>
  explicit IndirectOptional(std::in_place_t, Args&&... args) {
    (void)construct(std::forward<Args>(args)...);
  }

  template <class U, class... Args>
    requires detail::indirect_optional_initializer_list_factory<T, U, Args...>
  explicit IndirectOptional(std::in_place_t, std::initializer_list<U> init, Args&&... args) {
    (void)construct(init, std::forward<Args>(args)...);
  }

  IndirectOptional(const IndirectOptional& other) {
    if (other.has_value()) {
      (void)construct(*other);
    }
  }

  IndirectOptional(IndirectOptional&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

  ~IndirectOptional() { reset(); }

  auto operator=(const IndirectOptional& other) -> IndirectOptional& {
    if (this == &other) {
      return *this;
    }
    if (!other.has_value()) {
      reset();
      return *this;
    }
    if (has_value()) {
      *ptr_ = *other;
      return *this;
    }
    (void)construct(*other);
    return *this;
  }

  auto operator=(IndirectOptional&& other) noexcept -> IndirectOptional& {
    if (this == &other) {
      return *this;
    }

    reset();
    ptr_ = std::exchange(other.ptr_, nullptr);
    return *this;
  }

  auto operator=(std::nullopt_t) noexcept -> IndirectOptional& {
    reset();
    return *this;
  }

  template <class U>
    requires detail::indirect_optional_direct_value_lvalue_assignment<T, U>
  auto operator=(U&& value) -> IndirectOptional& {
    if (has_value()) {
      *ptr_ = std::forward<U>(value);
    } else {
      (void)construct(std::forward<U>(value));
    }
    return *this;
  }

  template <class U>
    requires detail::indirect_optional_direct_value_rvalue_assignment<T, U>
  auto operator=(U&& value) -> IndirectOptional& {
    if (has_value()) {
      *ptr_ = std::forward<U>(value);
    } else {
      (void)construct(std::forward<U>(value));
    }
    return *this;
  }


  [[nodiscard]] constexpr auto has_value() const noexcept -> bool { return ptr_ != nullptr; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] auto operator->() noexcept -> T* {
    assert(ptr_ != nullptr);
    return ptr_;
  }

  [[nodiscard]] auto operator->() const noexcept -> const T* {
    assert(ptr_ != nullptr);
    return ptr_;
  }

  [[nodiscard]] auto operator*() & -> T& {
    assert(ptr_ != nullptr);
    return *ptr_;
  }

  [[nodiscard]] auto operator*() const& -> const T& {
    assert(ptr_ != nullptr);
    return *ptr_;
  }

  [[nodiscard]] auto operator*() && -> T&& {
    assert(ptr_ != nullptr);
    return std::move(*ptr_);
  }

  [[nodiscard]] auto operator*() const&& -> const T&& {
    assert(ptr_ != nullptr);
    return std::move(*ptr_);
  }

  [[nodiscard]] auto value() & -> T& {
    if (!has_value()) {
      throw std::bad_optional_access{};
    }
    return *ptr_;
  }

  [[nodiscard]] auto value() const& -> const T& {
    if (!has_value()) {
      throw std::bad_optional_access{};
    }
    return *ptr_;
  }

  [[nodiscard]] auto value() && -> T&& {
    if (!has_value()) {
      throw std::bad_optional_access{};
    }
    return std::move(*ptr_);
  }

  [[nodiscard]] auto value() const&& -> const T&& {
    if (!has_value()) {
      throw std::bad_optional_access{};
    }
    return std::move(*ptr_);
  }

  template <class U>
    requires detail::indirect_optional_value_or_lvalue<T, U>
  [[nodiscard]] auto value_or(U&& default_value) const& -> T {
    return has_value() ? **this : static_cast<T>(std::forward<U>(default_value));
  }

  template <class U>
    requires detail::indirect_optional_value_or_rvalue<T, U>
  [[nodiscard]] auto value_or(U&& default_value) && -> T {
    return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(default_value));
  }

  void reset() noexcept {
    T* const old_ptr = std::exchange(ptr_, nullptr);
    if (old_ptr == nullptr) {
      return;
    }

    std::destroy_at(old_ptr);
    deallocate_heap(old_ptr);
  }

  template <class... Args>
    requires detail::indirect_optional_factory<T, Args...>
  auto emplace(Args&&... args) -> T& {
    reset();
    return construct(std::forward<Args>(args)...);
  }

  template <class U, class... Args>
    requires detail::indirect_optional_initializer_list_factory<T, U, Args...>
  auto emplace(std::initializer_list<U> init, Args&&... args) -> T& {
    reset();
    return construct(init, std::forward<Args>(args)...);
  }

  void swap(IndirectOptional& other) noexcept {
    if (this == &other) {
      return;
    }

    using std::swap;
    swap(ptr_, other.ptr_);
  }

  friend void swap(IndirectOptional& lhs, IndirectOptional& rhs) noexcept(noexcept(lhs.swap(rhs))) { lhs.swap(rhs); }

  [[nodiscard]] friend auto operator==(const IndirectOptional& lhs, const IndirectOptional& rhs) -> bool {
    if (lhs.has_value() != rhs.has_value()) {
      return false;
    }
    if (!lhs.has_value()) {
      return true;
    }
    return *lhs == *rhs;
  }

  [[nodiscard]] friend auto operator==(const IndirectOptional& value, std::nullopt_t) noexcept -> bool {
    return !value.has_value();
  }

  [[nodiscard]] friend auto operator==(std::nullopt_t, const IndirectOptional& value) noexcept -> bool {
    return !value.has_value();
  }

  [[nodiscard]] friend auto operator==(const IndirectOptional& optional, const T& value) -> bool {
    return optional.has_value() && *optional == value;
  }

  [[nodiscard]] friend auto operator==(const T& value, const IndirectOptional& optional) -> bool {
    return optional == value;
  }

private:
  [[nodiscard]] static auto allocate_heap() -> T* {
    void* raw = ::operator new(sizeof(T), std::align_val_t{alignof(T)});
    return static_cast<T*>(raw);
  }

  static void deallocate_heap(T* value_ptr) noexcept { ::operator delete(value_ptr, std::align_val_t{alignof(T)}); }

  template <class... Args>
  auto construct(Args&&... args) -> T& {
    T* storage = allocate_heap();
    try {
      std::construct_at(storage, std::forward<Args>(args)...);
      ptr_ = storage;
      return *ptr_;
    } catch (...) {
      deallocate_heap(storage);
      throw;
    }
  }

  T* ptr_ = nullptr;
};

template <class T>
[[nodiscard]] auto operator!=(const IndirectOptional<T>& lhs, const IndirectOptional<T>& rhs) -> bool {
  return !(lhs == rhs);
}

template <class T>
[[nodiscard]] auto operator<(const IndirectOptional<T>& lhs, const IndirectOptional<T>& rhs) -> bool {
  if (!rhs.has_value()) {
    return false;
  }
  if (!lhs.has_value()) {
    return true;
  }
  return *lhs < *rhs;
}

template <class T>
[[nodiscard]] auto operator<(const IndirectOptional<T>&, std::nullopt_t) noexcept -> bool {
  return false;
}

template <class T>
[[nodiscard]] auto operator<(std::nullopt_t, const IndirectOptional<T>& value) noexcept -> bool {
  return value.has_value();
}

template <class T>
[[nodiscard]] auto operator<(const IndirectOptional<T>& optional, const T& value) -> bool {
  return !optional.has_value() || *optional < value;
}

template <class T>
[[nodiscard]] auto operator<(const T& value, const IndirectOptional<T>& optional) -> bool {
  return optional.has_value() && value < *optional;
}

template <class T>
[[nodiscard]] auto operator<=(const IndirectOptional<T>& lhs, const IndirectOptional<T>& rhs) -> bool {
  return !(rhs < lhs);
}

template <class T>
[[nodiscard]] auto operator<=(const IndirectOptional<T>& value, std::nullopt_t) noexcept -> bool {
  return !(std::nullopt < value);
}

template <class T>
[[nodiscard]] auto operator<=(std::nullopt_t, const IndirectOptional<T>& value) noexcept -> bool {
  return !(value < std::nullopt);
}

template <class T>
[[nodiscard]] auto operator<=(const IndirectOptional<T>& optional, const T& value) -> bool {
  return !(value < optional);
}

template <class T>
[[nodiscard]] auto operator<=(const T& value, const IndirectOptional<T>& optional) -> bool {
  return !(optional < value);
}

template <class T>
[[nodiscard]] auto operator>(const IndirectOptional<T>& lhs, const IndirectOptional<T>& rhs) -> bool {
  return rhs < lhs;
}

template <class T>
[[nodiscard]] auto operator>(const IndirectOptional<T>& value, std::nullopt_t) noexcept -> bool {
  return std::nullopt < value;
}

template <class T>
[[nodiscard]] auto operator>(std::nullopt_t, const IndirectOptional<T>& value) noexcept -> bool {
  return value < std::nullopt;
}

template <class T>
[[nodiscard]] auto operator>(const IndirectOptional<T>& optional, const T& value) -> bool {
  return value < optional;
}

template <class T>
[[nodiscard]] auto operator>(const T& value, const IndirectOptional<T>& optional) -> bool {
  return optional < value;
}

template <class T>
[[nodiscard]] auto operator>=(const IndirectOptional<T>& lhs, const IndirectOptional<T>& rhs) -> bool {
  return !(lhs < rhs);
}

template <class T>
[[nodiscard]] auto operator>=(const IndirectOptional<T>& value, std::nullopt_t) noexcept -> bool {
  return !(value < std::nullopt);
}

template <class T>
[[nodiscard]] auto operator>=(std::nullopt_t, const IndirectOptional<T>& value) noexcept -> bool {
  return !(std::nullopt < value);
}

template <class T>
[[nodiscard]] auto operator>=(const IndirectOptional<T>& optional, const T& value) -> bool {
  return !(optional < value);
}

template <class T>
[[nodiscard]] auto operator>=(const T& value, const IndirectOptional<T>& optional) -> bool {
  return !(value < optional);
}

template <class T>
[[nodiscard]] auto operator!=(const IndirectOptional<T>& value, std::nullopt_t) noexcept -> bool {
  return !(value == std::nullopt);
}

template <class T>
[[nodiscard]] auto operator!=(std::nullopt_t, const IndirectOptional<T>& value) noexcept -> bool {
  return !(value == std::nullopt);
}

template <class T>
[[nodiscard]] auto operator!=(const IndirectOptional<T>& optional, const T& value) -> bool {
  return !(optional == value);
}

template <class T>
[[nodiscard]] auto operator!=(const T& value, const IndirectOptional<T>& optional) -> bool {
  return !(optional == value);
}

template <class T, class... Args>
  requires detail::indirect_optional_factory<T, Args...>
[[nodiscard]] auto make_indirect_optional(Args&&... args) -> IndirectOptional<T> {
  return IndirectOptional<T>(std::in_place, std::forward<Args>(args)...);
}

template <class T, class U, class... Args>
  requires detail::indirect_optional_initializer_list_factory<T, U, Args...>
[[nodiscard]] auto make_indirect_optional(std::initializer_list<U> init, Args&&... args) -> IndirectOptional<T> {
  return IndirectOptional<T>(std::in_place, init, std::forward<Args>(args)...);
}

}  // namespace fleaux::common
