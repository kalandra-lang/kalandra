#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/Syntax.h"
#include <expected>
#include <map>
namespace kalandra {
enum class GpuBackend { Metal, Vulkan };
std::expected<std::map<std::string, std::string>, Diagnostic>
emitGpuSupervisor(Module, ArithmeticMode, bool testCpuExports = false,
                  GpuBackend = GpuBackend::Metal);
} // namespace kalandra
