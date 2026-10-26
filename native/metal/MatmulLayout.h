#pragma once
#include <cstdint>

namespace kalandra::runtime::metal {
// Performance policy for the measured Apple M2 Max, not a language limit.
// Callers enable this profile only after validating the complete kernel bundle.
enum class MatmulLayout { Scalar, Rows2, Quad };
// Contraction-parallel profile. More rows regress against shared multi-row
// tiles; narrow/short and non-32-leaf contractions keep the original kernels.
constexpr bool useSkinnyMatmul(std::uint32_t rows, std::uint32_t inner,
                               std::uint32_t columns) noexcept {
  return rows >= 1 && rows <= 2 && inner >= 512 && inner <= 16384 && inner % 32 == 0 &&
         columns >= 256 && columns <= 16384;
}
// A bounded specialization; all other contractions keep the general kernels.
constexpr bool useRegisterMatmul(std::uint32_t rows, std::uint32_t inner,
                                 std::uint32_t columns) noexcept {
  return rows >= 16 && columns >= 16 && inner >= 512 && inner <= 4096 && inner % 64 == 0 &&
         std::uint64_t(rows) * columns >= 256u * 256u;
}
// Measured M2 Max tile-order profile: output at least 1M cells and
// combined input storage at least 16 MiB. Selection happens on the host so
// smaller shapes retain the original shader. Bounds keep shader indexing safe.
constexpr bool useGroupedMatmul(std::uint32_t rows, std::uint32_t inner,
                                std::uint32_t columns) noexcept {
  return rows >= 512 && rows <= 65535 && columns >= 512 && columns <= 65535 &&
         useRegisterMatmul(rows, inner, columns) &&
         std::uint64_t(rows) * columns >= 1024u * 1024u &&
         std::uint64_t(inner) * (std::uint64_t(rows) + columns) >= 4u * 1024u * 1024u;
}
constexpr MatmulLayout selectMatmulLayout(std::uint32_t rows, std::uint32_t inner,
                                          std::uint32_t columns) noexcept {
  const auto cells = std::uint64_t(rows) * columns;
  if (rows < 2 || inner < 128 || inner > 1025 || cells < 256u * 256u || cells > 513u * 513u)
    return MatmulLayout::Scalar;
  if (inner >= 256 && rows >= 128 && columns >= 128 && cells >= 512u * 512u)
    return MatmulLayout::Quad;
  return MatmulLayout::Rows2;
}
constexpr std::uint64_t matmulWorkItems(MatmulLayout layout, std::uint32_t rows,
                                        std::uint32_t columns) noexcept {
  const auto tileRows =
      layout == MatmulLayout::Scalar ? std::uint64_t(rows) : (std::uint64_t(rows) + 1) / 2;
  const auto tileColumns =
      layout == MatmulLayout::Quad ? (std::uint64_t(columns) + 1) / 2 : std::uint64_t(columns);
  return tileRows * tileColumns;
}
constexpr const char *matmulEntryName(MatmulLayout layout) noexcept {
  return layout == MatmulLayout::Rows2  ? "kalandra_matmul_2x1"
         : layout == MatmulLayout::Quad ? "kalandra_matmul_2x2"
                                        : "kalandra_matmul";
}
} // namespace kalandra::runtime::metal
