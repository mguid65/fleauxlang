#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/embed/vm_host.hpp"

namespace fleaux::embed {

enum class BindingErrorCode {
  kDuplicateSymbol,
  kInvalidSymbol,
  kInvalidSignature,
  kNotFound,
  kInternal,
};

struct BindingError {
  BindingErrorCode code{BindingErrorCode::kInternal};
  std::string message{};
  std::string symbol{};
};

using BindingResult = tl::expected<void, BindingError>;

struct ArityRange {
  std::size_t min{1};
  std::optional<std::size_t> max{1};
};

struct BindingSignature {
  ArityRange arity{};
  std::string description{};
};

struct BindingContext {
  VmHost* host{nullptr};
  std::string_view symbol{};
};

using NativeInvokeResult = HostResult<VmValue>;
using NativeCallable = std::function<NativeInvokeResult(const BindingContext&, const VmValue&)>;

struct NativeBinding {
  std::string symbol{};
  BindingSignature signature{};
  NativeCallable callable{};
};

class NativeBindingRegistry {
public:
  [[nodiscard]] auto register_callable(NativeBinding binding) -> BindingResult;
  [[nodiscard]] auto unregister_callable(std::string_view symbol) -> bool;
  [[nodiscard]] auto has_callable(std::string_view symbol) const -> bool;
  [[nodiscard]] auto clear() -> std::size_t;
  [[nodiscard]] auto size() const -> std::size_t;
  [[nodiscard]] auto snapshot_symbols() const -> std::vector<std::string>;

  [[nodiscard]] auto find_callable(std::string_view symbol) const -> const NativeBinding*;

private:
  std::vector<NativeBinding> bindings_{};
};

}  // namespace fleaux::embed

