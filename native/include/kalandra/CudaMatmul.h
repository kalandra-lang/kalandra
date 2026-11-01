#pragma once
#include <cstdint>
#include <limits>
namespace kalandra {
// V3 catalogs have exactly four reflected descriptor entry points. Partition
// count and accumulation tree are invariant across geometry and byte budgets.
inline constexpr const char *cudaMatmulEntry =
    "kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1";
inline constexpr unsigned cudaMatmulParts = 5;
inline constexpr bool cudaMatmulCatalogShape(std::uint64_t m, std::uint64_t n, std::uint64_t k) {
  // Preserve V2's aligned accelerated domain. Tails/empty products still use
  // the fixed-tree GPU companion; widened support is a separate policy change.
  constexpr auto limit = std::uint64_t(std::numeric_limits<std::int32_t>::max());
  return m && n && k && !(m % 64) && !(n % 64) && !(k % 16) && m <= limit - 255 &&
         n <= limit - 255 && k <= limit - 39 && m * n < limit && m * k < limit && k * n < limit;
}
inline constexpr bool cudaMatmulWide(std::uint64_t m, std::uint64_t n, std::uint64_t k) {
  // Wide output tiles amortize long products, but waste occupancy for smaller
  // outputs. Keep the narrow tile for medium-area, long-contraction products.
  return m >= 1024 && n >= 1024 && (k <= 1024 || (m >= 2048 && n >= 2048));
}
} // namespace kalandra
