#pragma once
#include <cstdint>
namespace kalandra::runtime {
inline constexpr std::uint64_t formatDescriptorBytes = 24;
// Private v1 formatting bridge. Independent of NumericType's internal numbering.
enum class FormatScalarKind : std::uint32_t {
  I8 = 1, I16, I32, I64, U8, U16, U32, U64, F32, F64, Bool
};
enum class FormatPresentation : std::uint32_t { Default, Fixed, HexLower };
enum class FormatAlignment : std::uint32_t { None, Left, Right };
} // namespace kalandra::runtime
