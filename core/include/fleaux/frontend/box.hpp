#pragma once

#include <cassert>
#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace fleaux::frontend {

namespace detail {

template <class T>
concept complete_type = requires { sizeof(T); };

template <class T, class... Args>
concept box_factory = complete_type<T> && std::constructible_from<T, Args&&...>;

}  // namespace detail

template <class T>
class Box {
public:
  explicit constexpr Box() noexcept : ptr_(std::make_unique<T>()){};

  template <class... Args>
    requires detail::box_factory<T, Args...>
  explicit Box(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
      : ptr_(std::make_unique<T>(std::forward<Args>(args)...)) {}

  Box(const T& value) : ptr_(std::make_unique<T>(value)) {}
  Box(T&& value) : ptr_(std::make_unique<T>(std::move(value))) {}

  Box(const Box& other) : ptr_(clone_ptr(other)) {}
  Box(Box&&) noexcept = default;

  auto operator=(const Box& other) -> Box& {
    if (this != &other) {
      ptr_ = clone_ptr(other);
    }
    return *this;
  }

  auto operator=(Box&&) noexcept -> Box& = default;

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

  [[nodiscard]] auto operator->() noexcept -> T* { return std::to_address(ptr_); }

  [[nodiscard]] auto operator->() const noexcept -> const T* { return std::to_address(ptr_); }

  template <class... Args>
    requires detail::box_factory<T, Args...>
  auto emplace(Args&&... args) -> T& {
    ptr_ = std::make_unique<T>(std::forward<Args>(args)...);
    return *ptr_;
  }

private:
  [[nodiscard]] static auto clone_ptr(const Box& other) -> std::unique_ptr<T> {
    assert(other.ptr_ != nullptr);
    return std::make_unique<T>(*other.ptr_);
  }

  std::unique_ptr<T> ptr_{nullptr};
};

template <class T, class... Args>
  requires detail::box_factory<T, Args...>
[[nodiscard]] auto make_box(Args&&... args) -> Box<T> {
  return Box<T>(std::forward<Args>(args)...);
}

}  // namespace fleaux::frontend
