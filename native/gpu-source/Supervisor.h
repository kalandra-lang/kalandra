#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/Optimization.h"
#include "kalandra/Syntax.h"
#include "kalandra/TypeCapabilities.h"
#include <expected>
#include <map>
namespace kalandra {
enum class GpuBackend { Metal, Vulkan, Cuda };
constexpr CapabilityTarget capabilityTarget(GpuBackend backend) {
  switch (backend) {
  case GpuBackend::Metal:
    return CapabilityTarget::Metal;
  case GpuBackend::Vulkan:
    return CapabilityTarget::Vulkan;
  case GpuBackend::Cuda:
    return CapabilityTarget::Cuda;
  }
  return static_cast<CapabilityTarget>(255);
}
std::expected<std::map<std::string, std::string>, Diagnostic>
emitGpuSupervisor(Module, ArithmeticMode, bool testCpuExports = false,
                  GpuBackend = GpuBackend::Metal, bool application = false,
                  Optimization level = Optimization::O0, bool bundledHost = false);
} // namespace kalandra
