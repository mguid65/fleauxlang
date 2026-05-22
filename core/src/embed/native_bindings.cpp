#include "fleaux/embed/native_bindings.hpp"

#include <algorithm>
#include <cctype>

namespace fleaux::embed {
namespace {

[[nodiscard]] auto is_valid_symbol(const std::string_view symbol) -> bool {
  if (symbol.empty()) {
    return false;
  }

  return std::ranges::none_of(symbol, [](const unsigned char ch) { return std::isspace(ch) != 0; });
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

  bindings_.push_back(std::move(binding));
  return {};
}

auto NativeBindingRegistry::unregister_callable(const std::string_view symbol) -> bool {
  const auto it = std::ranges::find_if(
      bindings_, [symbol](const NativeBinding& binding) -> bool { return binding.symbol == symbol; });
  if (it == bindings_.end()) {
    return false;
  }

  bindings_.erase(it);
  return true;
}

auto NativeBindingRegistry::has_callable(const std::string_view symbol) const -> bool {
  return std::ranges::any_of(bindings_,
                             [symbol](const NativeBinding& binding) -> bool { return binding.symbol == symbol; });
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
  for (const auto& binding : bindings_) {
    symbols.push_back(binding.symbol);
  }
  return symbols;
}

auto NativeBindingRegistry::find_callable(const std::string_view symbol) const
    -> std::optional<std::reference_wrapper<const NativeBinding>> {
  const auto it = std::ranges::find_if(
      bindings_, [symbol](const NativeBinding& binding) -> bool { return binding.symbol == symbol; });
  if (it == bindings_.end()) {
    return std::nullopt;
  }

  return std::cref(*it);
}

}  // namespace fleaux::embed
