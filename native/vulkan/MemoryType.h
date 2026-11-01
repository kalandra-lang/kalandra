#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace kalandra::runtime::vulkan {
// Buffers remain persistently mapped. Prefer mappable device-local memory so
// repeated shader reads do not cross PCIe; preserve a portable host fallback.
inline std::uint32_t mappedMemoryType(const VkPhysicalDeviceMemoryProperties &memory,
                                      std::uint32_t compatible, bool readback = false) noexcept {
  std::uint32_t selected = UINT32_MAX;
  unsigned best = 0;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; ++i) {
    const auto flags = memory.memoryTypes[i].propertyFlags;
    if (!(compatible & (UINT32_C(1) << i)) ||
        !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        // Neither protectedMemory nor deviceCoherentMemory is enabled.
        // VUID-vkAllocateMemory-deviceCoherentMemory-02790 applies even when
        // the extension's memory types are advertised by the physical device.
        (flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD)))
      continue;
    if (readback && !(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
      continue;
    const unsigned score = ((!readback && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ? 4 : 0) +
                           ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                           ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
    if (selected == UINT32_MAX || score > best) {
      selected = i;
      best = score;
    }
  }
  return selected;
}
}
