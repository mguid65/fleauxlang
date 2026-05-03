#pragma once

#include <cassert>
#include <memory>
#include <utility>

namespace fleaux::frontend {

template <class T>
class Box {
public:
  constexpr Box() noexcept = default;

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

  [[nodiscard]] auto operator->() noexcept -> T* {
    assert(ptr_ != nullptr);
    return ptr_.get();
  }

  [[nodiscard]] auto operator->() const noexcept -> const T* {
    assert(ptr_ != nullptr);
    return ptr_.get();
  }

  void reset() noexcept { ptr_.reset(); }

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
  Box<T> out;
  out.emplace(std::forward<Args>(args)...);
  return out;
}

}  // namespace fleaux::frontend
