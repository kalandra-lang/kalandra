#pragma once
#include "kalandra/NumericType.h"
#include "kalandra/TensorTransform.h"
#include <string>
namespace kalandra {
inline std::string vulkanTransformKernelName(TensorTransformKind kind, NumericType type) {
  auto description = describeNumeric(type);
  if (!description)
    return {};
  return "kalandra_tensor_transform_v1_" + std::string(tensorTransformName(kind)) + "_" +
         std::string(description->name) + "_vk_groups_v1";
}
} // namespace kalandra
