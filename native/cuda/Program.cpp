#include "Program.h"
#include "kalandra/NumericKernelName.h"
#include <algorithm>
#include <cstring>
#include <new>
namespace kalandra::runtime::cuda {
namespace {
inline std::string backendTransformKernelName(TensorTransformKind kind, NumericType type) {
  return std::string("kalandra_tensor_transform_v1_") + std::string(tensorTransformName(kind)) +
         "_" + std::string(describeNumeric(type)->name) + "_cuda_desc_v1";
}
constexpr const char *backendKernelSuffix = "_cuda_desc_v1";
} // namespace
#include "../gpu-runtime/ProgramImpl.inc"
} // namespace kalandra::runtime::cuda
