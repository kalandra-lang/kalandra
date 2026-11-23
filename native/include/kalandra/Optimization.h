#pragma once
#include <array>
#include <optional>
#include <string_view>

namespace kalandra {
enum class Optimization { O0, O1, O2, O3 };
inline constexpr std::array optimizationFlags{"-O0", "-O1", "-O2", "-O3"};
constexpr const char *optimizationFlag(Optimization level) {
  return optimizationFlags[static_cast<unsigned>(level)];
}
constexpr std::optional<Optimization> parseOptimization(std::string_view flag) {
  for (unsigned i = 0; i < optimizationFlags.size(); ++i)
    if (flag == optimizationFlags[i])
      return static_cast<Optimization>(i);
  return {};
}
} // namespace kalandra
