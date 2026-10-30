#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kalandra::cli {
// Private executable-entry descriptor, not a public library or wire ABI.
// The active prefix uses [rank sizes][rank strides], as in a ranked memref.
inline constexpr std::uint64_t stringArrayElement = 0x80000;
inline constexpr std::uint64_t boolArrayElement = 0x70000;
inline constexpr std::size_t maximumCollectionRank = 128;
struct CollectionDescriptor {
  void *allocated = nullptr;
  void *aligned = nullptr;
  std::int64_t offset = 0;
  std::array<std::int64_t, 2 * maximumCollectionRank> dimensions{};
};
static_assert(sizeof(void *) == 8 && std::is_standard_layout_v<CollectionDescriptor>);
static_assert(offsetof(CollectionDescriptor, dimensions) == 24);
} // namespace kalandra::cli
