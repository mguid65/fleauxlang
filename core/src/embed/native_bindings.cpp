#include "fleaux/embed/native_bindings.hpp"

#include <algorithm>
#include <cctype>

namespace fleaux::embed {
namespace {

[[nodiscard]] auto is_valid_symbol(const std::string_view symbol) -> bool {
  if (symbol.empty()) {
    return false;
  }

  return std::ranges::none_of(symbol, [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
}

[[nodiscard]] auto has_valid_arity(const BindingSignature& signature) -> bool {
  if (!signature.arity.max.has_value()) {
    return true;
  }
  return *signature.arity.max >= signature.arity.min;
}

}  // namespace

auto NativeBindingRegistry::register_callable(NativeBinding binding) -> BindingResult {
  if (!is_valid_symbol(binding.symbol)) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInvalidSymbol,
        .message = "Binding symbol must be non-empty and contain no whitespace.",
        .symbol = std::move(binding.symbol),
    });
  }

  if (!has_valid_arity(binding.signature)) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInvalidSignature,
        .message = "Binding signature arity is invalid.",
        .symbol = std::move(binding.symbol),
    });
  }

  if (!static_cast<bool>(binding.callable)) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInvalidSignature,
        .message = "Binding callable is empty.",
        .symbol = std::move(binding.symbol),
    });
  }

  if (has_callable(binding.symbol)) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kDuplicateSymbol,
        .message = "Binding symbol already exists.",
        .symbol = std::move(binding.symbol),
    });
  }

  bindings_.push_back(StoredBinding{.binding = std::move(binding), .binary_callable = {}});
  return {};
}

auto NativeBindingRegistry::register_callable(NativeBinding binding, NativeBinaryCallable binary_callable) -> BindingResult {
  if (!static_cast<bool>(binary_callable)) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInvalidSignature,
        .message = "Binding binary callable is empty.",
        .symbol = std::move(binding.symbol),
    });
  }

  if (auto registered = register_callable(std::move(binding)); !registered.has_value()) {
    return registered;
  }

  bindings_.back().binary_callable = std::move(binary_callable);
  return {};
}

auto NativeBindingRegistry::unregister_callable(const std::string_view symbol) -> bool {
  const auto it = std::ranges::find_if(
      bindings_, [symbol](const StoredBinding& binding) -> bool { return binding.binding.symbol == symbol; });
  if (it == bindings_.end()) {
    return false;
  }

  bindings_.erase(it);
  return true;
}

auto NativeBindingRegistry::has_callable(const std::string_view symbol) const -> bool {
  return std::ranges::any_of(bindings_,
                             [symbol](const StoredBinding& binding) -> bool { return binding.binding.symbol == symbol; });
}

auto NativeBindingRegistry::has_binary_callable(const std::string_view symbol) const -> bool {
  return std::ranges::any_of(bindings_, [symbol](const StoredBinding& binding) -> bool {
    return binding.binding.symbol == symbol && static_cast<bool>(binding.binary_callable);
  });
}

auto NativeBindingRegistry::clear() -> std::size_t {
  const std::size_t removed = bindings_.size();
  bindings_.clear();
  return removed;
}

auto NativeBindingRegistry::size() const -> std::size_t { return bindings_.size(); }

auto NativeBindingRegistry::snapshot_symbols() const -> std::vector<std::string> {
  std::vector<std::string> symbols;
  symbols.reserve(bindings_.size());
  for (const auto& [binding, binary_callable] : bindings_) {
    (void) binary_callable;
    symbols.push_back(binding.symbol);
  }
  return symbols;
}

auto NativeBindingRegistry::find_callable(const std::string_view symbol) const
    -> std::optional<std::reference_wrapper<const NativeBinding>> {
  const auto it = std::ranges::find_if(
      bindings_, [symbol](const StoredBinding& binding) -> bool { return binding.binding.symbol == symbol; });
  if (it == bindings_.end()) {
    return std::nullopt;
  }

  return std::cref(it->binding);
}

auto NativeBindingRegistry::find_binary_callable(const std::string_view symbol) const
    -> std::optional<std::reference_wrapper<const NativeBinaryCallable>> {
  const auto it = std::ranges::find_if(
      bindings_, [symbol](const StoredBinding& binding) -> bool { return binding.binding.symbol == symbol; });
  if (it == bindings_.end() || !static_cast<bool>(it->binary_callable)) {
    return std::nullopt;
  }

  return std::cref(it->binary_callable);
}

}  // namespace fleaux::embed
