#pragma once
#include "kalandra/Backend.h"

namespace kalandra {
std::expected<BackendOutput, std::string> lowerTaskNative(const TypedModule &,
                                                          std::optional<std::string>,
                                                          ArithmeticMode, const CpuTarget &,
                                                          Optimization);
std::expected<BackendOutput, std::string> lowerTaskNativeExports(const TypedModule &,
                                                                 const std::vector<std::string> &,
                                                                 ArithmeticMode, const CpuTarget &,
                                                                 Optimization);
} // namespace kalandra
