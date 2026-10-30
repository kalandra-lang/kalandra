#pragma once
#include <optional>
#include <string_view>
namespace kalandra {
enum class ArithmeticMode { Hardware, Software, HardwareWithSoftwareFallback };
enum class MatmulPolicy { Optimized, FixedTree };
inline std::optional<MatmulPolicy> parseMatmulPolicy(std::string_view value) {
  if (value == "optimized")
    return MatmulPolicy::Optimized;
  if (value == "fixed-tree")
    return MatmulPolicy::FixedTree;
  return {};
}
inline std::string_view matmulPolicyName(MatmulPolicy policy) {
  return policy == MatmulPolicy::FixedTree ? "fixed-tree" : "optimized";
}
inline std::optional<ArithmeticMode> parseArithmeticMode(std::string_view value) {
  if (value == "hardware")
    return ArithmeticMode::Hardware;
  if (value == "software")
    return ArithmeticMode::Software;
  if (value == "hardware-with-software-fallback")
    return ArithmeticMode::HardwareWithSoftwareFallback;
  return {};
}
inline std::string_view arithmeticModeName(ArithmeticMode mode) {
  switch (mode) {
  case ArithmeticMode::Hardware:
    return "hardware";
  case ArithmeticMode::Software:
    return "software";
  case ArithmeticMode::HardwareWithSoftwareFallback:
    return "hardware-with-software-fallback";
  }
  return "invalid";
}
} // namespace kalandra
