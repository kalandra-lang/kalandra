#pragma once
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include <array>
#include <bit>
#include <cstdint>
#include <vector>
namespace float_proof {
struct Pair {
  std::uint32_t a, b;
};
inline bool nan(std::uint32_t bits) { return (bits & 0x7fffffff) > 0x7f800000; }
inline bool equal(std::uint32_t a, std::uint32_t b) { return a == b || (nan(a) && nan(b)); }
inline std::uint32_t oracle(Pair pair, unsigned operation) {
  llvm::APFloat a(llvm::APFloat::IEEEsingle(), llvm::APInt(32, pair.a));
  llvm::APFloat b(llvm::APFloat::IEEEsingle(), llvm::APInt(32, pair.b));
  auto result = a;
  if (operation == 0)
    result.add(b, llvm::APFloat::rmNearestTiesToEven);
  else if (operation == 3)
    result.subtract(b, llvm::APFloat::rmNearestTiesToEven);
  else if (operation == 4)
    result.changeSign();
  else if (operation == 5)
    result.divide(b, llvm::APFloat::rmNearestTiesToEven);
  else {
    result.multiply(b, llvm::APFloat::rmNearestTiesToEven);
    if (operation == 2)
      result.add(a, llvm::APFloat::rmNearestTiesToEven);
  }
  return result.bitcastToAPInt().getZExtValue();
}
inline std::uint32_t hardware(Pair pair, unsigned operation) {
  float a = std::bit_cast<float>(pair.a), b = std::bit_cast<float>(pair.b);
  if (operation == 0)
    return std::bit_cast<std::uint32_t>(a + b);
  if (operation == 3)
    return std::bit_cast<std::uint32_t>(a - b);
  if (operation == 4)
    return std::bit_cast<std::uint32_t>(-a);
  if (operation == 5)
    return std::bit_cast<std::uint32_t>(a / b);
  volatile float product = a * b;
  if (operation == 1)
    return std::bit_cast<std::uint32_t>(float(product));
  return std::bit_cast<std::uint32_t>(product + a);
}
// Division-specific neighbors around underflow/overflow and quotient rounding.
// Keep the established shared corpus stable for the other operation proofs.
inline std::vector<Pair> divisionCases() {
  std::vector<Pair> result;
  for (unsigned signs = 0; signs < 4; ++signs)
    for (std::uint32_t i = 0; i < 1024; ++i)
      for (auto denominator : {0x3f000000u, 0x3f000001u, 0x3f7fffffu, 0x3f800001u, 0x40000000u,
                               0x40000001u, 0x40400000u, 0x7f7fffffu})
        for (auto numerator : {i + 1, 0x3f800000u + i, 0x7f7fffffu - i})
          result.push_back({numerator | ((signs & 1) ? 0x80000000u : 0u),
                            denominator | ((signs & 2) ? 0x80000000u : 0u)});
  return result;
}
inline std::vector<Pair> corpus() {
  std::vector<Pair> pairs;
  std::vector<std::uint32_t> edges;
  for (std::uint32_t sign : {0u, 0x80000000u})
    for (std::uint32_t exponent : {0u, 1u, 2u, 63u, 64u, 65u, 125u, 126u, 127u, 128u, 129u, 190u,
                                   191u, 192u, 252u, 253u, 254u, 255u})
      for (std::uint32_t fraction : {0u, 1u, 0x3fffffu, 0x400000u, 0x7ffffeu, 0x7fffffu})
        edges.push_back(sign | (exponent << 23) | fraction);
  for (auto a : edges)
    for (auto b : edges)
      pairs.push_back({a, b});
  // Cover every pair of encoded exponents, four sign combinations, and
  // significands straddling binade and halfway boundaries.
  for (std::uint32_t a = 0; a < 256; ++a)
    for (std::uint32_t b = 0; b < 256; ++b)
      for (unsigned signs = 0; signs < 4; ++signs)
        pairs.push_back(
            {((signs & 1) ? 0x80000000u : 0u) | (a << 23) | ((a & 1) ? 0x7fffffu : 1u),
             ((signs & 2) ? 0x80000000u : 0u) | (b << 23) | ((b & 1) ? 0x400001u : 0x3fffffu)});
  // Exercise every subnormal leading-bit position and cancellation distance,
  // including shifts that stop at the exponent floor rather than the hidden bit.
  for (unsigned bit = 0; bit < 23; ++bit)
    for (std::uint32_t sign : {0u, 0x80000000u}) {
      const auto subnormal = sign | (1u << bit);
      for (std::uint32_t scale : {0x00800000u, 0x3f000000u, 0x3f800000u, 0x40000000u})
        pairs.push_back({subnormal, scale});
      for (std::uint32_t exponent = 1; exponent < 255; ++exponent) {
        const auto base = exponent << 23;
        pairs.push_back({sign | base | (1u << bit), (sign ^ 0x80000000u) | base});
      }
    }
  std::uint32_t seed = 0x42514e49;
  auto next = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  for (unsigned i = 0; i < 262144; ++i)
    pairs.push_back({next(), next()});
  for (unsigned i = 0; i < 4096; ++i) {
    auto a = next() & 0x7fffffff;
    for (unsigned delta : {0u, 1u, 2u})
      pairs.push_back({a, (a + delta) ^ 0x80000000u});
  }
  for (auto pair : std::array<Pair, 6>{{{1, 0x3f800000},
                                        {0x80000000, 0x3f800000},
                                        {0x3f800001, 0xbf7ffffe},
                                        {0x00800000, 0x3f7fffff},
                                        {0x7f7fffff, 0x3f800001},
                                        {0x80000001, 0x3f000000}}})
    pairs.push_back(pair);
  return pairs;
}
} // namespace float_proof
