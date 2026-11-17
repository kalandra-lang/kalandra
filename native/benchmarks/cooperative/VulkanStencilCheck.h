#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace kalandra::benchmark {
inline std::uint32_t orderedFloatBits(std::uint32_t bits) noexcept {
  return bits ^ ((0u - (bits >> 31)) | 0x80000000u);
}
using StencilInterval = std::array<std::uint32_t, 2>;

// The fixture's finite quarter-valued inputs make both additions exact.
// Calculate the original 2.5-ULP acceptance interval once, before GPU warmup.
// Each batch can then check every element without lengthy CPU/libm gaps.
inline StencilInterval stencilInterval(float left, float middle, float right, float reference) {
  const long double exact = (static_cast<long double>(left) + middle + right) / 3.0L;
  const auto ordered = [](float value) {
    return orderedFloatBits(std::bit_cast<std::uint32_t>(value));
  };
  if (exact == 0)
    return {ordered(reference), ordered(reference)}; // Preserve the exact zero sign.
  int exponent = 0;
  std::frexp(std::fabs(exact), &exponent);
  auto ulp = std::ldexp(1.0L, exponent - 24);
  if (std::fabs(exact) == std::ldexp(1.0L, exponent - 1))
    ulp *= 0.5L;
  const auto accepts = [&](float value) {
    return std::isfinite(value) && std::fabs(static_cast<long double>(value) - exact) <= 2.5L * ulp;
  };
  const auto infinity = std::numeric_limits<float>::infinity();
  auto lower = static_cast<float>(exact - 2.5L * ulp);
  auto upper = static_cast<float>(exact + 2.5L * ulp);
  if (!accepts(lower))
    lower = std::nextafter(lower, infinity);
  if (!accepts(upper))
    upper = std::nextafter(upper, -infinity);
  // Independently recheck both included endpoints and excluded neighbors using
  // the original long-double criterion. The accepted finite set is contiguous.
  if (!accepts(lower) || !accepts(upper) || ordered(lower) > ordered(upper) ||
      accepts(std::nextafter(lower, -infinity)) || accepts(std::nextafter(upper, infinity)))
    throw std::runtime_error("stencil ULP interval disagrees with the reference criterion");
  return {ordered(lower), ordered(upper)};
}
} // namespace kalandra::benchmark
