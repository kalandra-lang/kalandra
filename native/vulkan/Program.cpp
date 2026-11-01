#include "Program.h"
#include "kalandra/NumericKernelName.h"
#include <algorithm>
#include <cstring>
#include <new>
namespace kalandra::runtime::vulkan {
namespace {
inline std::string backendTransformKernelName(TensorTransformKind kind, NumericType type) {
  return vulkanTransformKernelName(kind, type);
}
constexpr const char *backendKernelSuffix = "_vk_flat_v1";
}
#include "../gpu-runtime/ProgramImpl.inc"
} // namespace kalandra::runtime::vulkan
