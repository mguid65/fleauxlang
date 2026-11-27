#pragma once

// FNV-1a, the 64-bit hash of the bytecode's source text hashes, checksums, and builtin table hash.

#include <cstdint>
#include <span>
#include <string_view>

namespace fleaux::common {

inline constexpr std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;

// The hash of `text`, continuing from `hash`.
[[nodiscard]] constexpr auto fnv1a(const std::string_view text, std::uint64_t hash = kFnv1aOffsetBasis) noexcept -> std::uint64_t {
  for (const char ch : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= kFnv1aPrime;
  }
  return hash;
}

// The hash of `bytes`, continuing from `hash`.
[[nodiscard]] constexpr auto fnv1a(const std::span<const std::uint8_t> bytes, std::uint64_t hash = kFnv1aOffsetBasis) noexcept
    -> std::uint64_t {
  for (const std::uint8_t byte : bytes) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnv1aPrime;
  }
  return hash;
}

}  // namespace fleaux::common
