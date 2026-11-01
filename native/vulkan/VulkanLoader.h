#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
namespace kalandra::runtime::vulkan {
// Immutable after one thread-safe initialization. No SDK loader import is
// required by consumers; the OS loader must provide every used core entry.
struct VulkanApi {
#define KALANDRA_VULKAN_FUNCTION(name) PFN_##name name = nullptr;
#include "VulkanFunctions.inc"
#undef KALANDRA_VULKAN_FUNCTION
  bool available = false;
};
const VulkanApi &vulkanApi() noexcept;
} // namespace kalandra::runtime::vulkan
