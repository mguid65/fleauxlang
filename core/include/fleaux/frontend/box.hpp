#pragma once

#include <cassert>
#include <memory>
#include <utility>

namespace fleaux::frontend {

template <class T>
class Box {
public:
  // TODO: Giving the box the ability to even be nullable is an issue,
  // TODO: but right now we are dependent on this behavior and if it is default constructed here with a default T
  // TODO: then we will infinitely recurse many of the AST and IR nodes because we check it with operator::bool()
  // TODO: which always returns true if we init ptr_ here with make_unique<T>().
  // TODO: Basically a fault on myself for not using Box idiomatically.
  explicit constexpr Box() noexcept = default;

  // TODO: COMMENT BY PATRICK: CONSIDER: constexpr Box(std::nullptr_t) noexcept {}

  template <class... Args>
    requires std::constructible_from<T, Args...>
  explicit Box(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
      : ptr_(std::make_unique<T>(std::forward<Args>(args)...)) {}

  Box(const T& value) : ptr_(std::make_unique<T>(value)) {}
  Box(T&& value) : ptr_(std::make_unique<T>(std::move(value))) {}

  Box(const Box& other) : ptr_(other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr) {}
  Box(Box&&) noexcept = default;

  auto operator=(const Box& other) -> Box& {
    if (this != &other) {
      ptr_ = other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr;
    }
    return *this;
  }

  auto operator=(Box&&) noexcept -> Box& = default;

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }

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
  auto emplace(Args&&... args) -> T& {
    ptr_ = std::make_unique<T>(std::forward<Args>(args)...);
    return *ptr_;
  }

private:
  std::unique_ptr<T> ptr_{nullptr};
};

template <class T, class... Args>
[[nodiscard]] auto make_box(Args&&... args) -> Box<T> {
  return Box<T>(std::forward<Args>(args)...);
}

}  // namespace fleaux::frontend
