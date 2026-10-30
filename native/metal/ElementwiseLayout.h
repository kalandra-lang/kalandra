#pragma once
#include <cstdint>

namespace kalandra::runtime::metal {
// Metal's direct buffer argument table has 31 entries per kernel. Outputs and
// the scalar/count parameter block use two. This is a backend binding limit,
// not a language parameter limit. Larger entries use a Tier 2 argument buffer.
inline constexpr std::uint32_t directBufferEntries = 31;
enum class ElementwiseBinding { Direct, ArgumentBuffer };
inline constexpr bool indirectElementwiseLayout(std::uint64_t tensors, std::uint64_t scalars) {
  return tensors >= 1 && tensors <= UINT32_MAX - 2 && scalars < UINT32_MAX;
}
inline constexpr bool directElementwiseLayout(std::uint64_t tensors, std::uint64_t scalars) {
  return tensors >= 1 && tensors <= directBufferEntries - 2 && scalars < UINT32_MAX;
}
} // namespace kalandra::runtime::metal
