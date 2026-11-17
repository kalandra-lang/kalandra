#include "VulkanStencilCheck.h"
#include <cstdio>
#include <vector>
using namespace kalandra::benchmark;
int main() {
  unsigned checked = 0;
  for (int numerator = -54; numerator <= 54; ++numerator)
    for (bool negativeZero : {false, true}) {
      if (negativeZero && numerator)
        continue;
      const float left = negativeZero ? -0.0f : float(numerator) * 0.25f;
      const float other = negativeZero ? -0.0f : 0.0f;
      const float reference = ((left + other) + other) / 3.0f;
      const auto interval = stencilInterval(left, other, other, reference);
      const long double exact = (static_cast<long double>(left) + other + other) / 3.0L;
      int exponent = 0;
      std::frexp(std::fabs(exact), &exponent);
      auto ulp = std::ldexp(1.0L, exponent - 24);
      if (std::fabs(exact) == std::ldexp(1.0L, exponent - 1))
        ulp *= 0.5L;
      std::vector<std::uint32_t> candidates{0,           0x80000000u, 0x7f800000u,
                                            0xff800000u, 0x7fc00001u, 0xffc00001u};
      const auto center = orderedFloatBits(std::bit_cast<std::uint32_t>(reference));
      for (int offset = -16; offset <= 16; ++offset) {
        const auto ordered = center + offset;
        candidates.push_back(ordered & 0x80000000u ? ordered ^ 0x80000000u : ~ordered);
      }
      for (auto bits : candidates) {
        const float value = std::bit_cast<float>(bits);
        const bool original =
            std::isfinite(value) &&
            (exact == 0 ? bits == std::bit_cast<std::uint32_t>(reference)
                        : std::fabs(static_cast<long double>(value) - exact) <= 2.5L * ulp);
        const auto ordered = orderedFloatBits(bits);
        const bool fast = ordered >= interval[0] && ordered <= interval[1];
        if (original != fast)
          return 1;
        ++checked;
      }
    }
  std::printf("PASS: %u acceptance decisions across all 109 possible exact sums, both zero signs, "
              "adjacent floats, infinities and NaNs\n",
              checked);
}
