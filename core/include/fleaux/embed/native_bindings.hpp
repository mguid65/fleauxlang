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

// BindingSignature currently records descriptive metadata for hosts, docs, and
// introspection. Native dispatch still forwards the raw VmValue argument to the
// binding callable and does not enforce the declared arity range.
struct BindingSignature {
  ArityRange arity{};
  std::string description{};
};

struct BindingContext {
  // Non-owning reference to the active host for the duration of this
  // invocation. Native bindings may use it to make additional host calls or
  // other mutating host operations while the callback is running.
  VmHost& host;
  std::string_view symbol{};
};

// Native callables receive the raw VmValue provided by the host. Hosts that
// want stricter argument validation should enforce it inside the callable or in
// a higher-level wrapper.
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

  // Returns a borrowed view of the stored binding, including its descriptive
  // BindingSignature metadata, or std::nullopt when the symbol is not
  // registered. The returned reference is invalidated by any later
  // register_callable(), unregister_callable(), or clear() call.
  [[nodiscard]] auto find_callable(std::string_view symbol) const
      -> std::optional<std::reference_wrapper<const NativeBinding>>;

private:
  std::vector<NativeBinding> bindings_{};
};

}  // namespace fleaux::embed

