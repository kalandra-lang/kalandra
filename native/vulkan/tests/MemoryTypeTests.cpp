#include "MemoryType.h"
#include <cstdlib>
using namespace kalandra::runtime::vulkan;
void check(bool value) {
  if (!value)
    std::abort();
}
int main() {
  constexpr auto visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  constexpr auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  constexpr auto local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkPhysicalDeviceMemoryProperties memory{};
  check(mappedMemoryType(memory, UINT32_MAX) == UINT32_MAX);
  memory.memoryTypeCount = 6;
  memory.memoryTypes[0].propertyFlags = local;
  memory.memoryTypes[1].propertyFlags = visible | coherent;
  memory.memoryTypes[2].propertyFlags = visible | coherent | cached;
  memory.memoryTypes[3].propertyFlags = visible | coherent | local;
  memory.memoryTypes[4].propertyFlags = visible | cached | local;
  memory.memoryTypes[5].propertyFlags = visible | coherent | local;
  check(mappedMemoryType(memory, 0) == UINT32_MAX);
  check(mappedMemoryType(memory, 1) == UINT32_MAX);
  check(mappedMemoryType(memory, UINT32_MAX) == 3); // Stable tie.
  check(mappedMemoryType(memory, 7) == 2); // Host fallback prefers cached.
  check(mappedMemoryType(memory, 3) == 1);
  check(mappedMemoryType(memory, 0x14) == 4); // Noncoherent VRAM is usable.
  memory.memoryTypes[3].propertyFlags |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
  check(mappedMemoryType(memory, UINT32_MAX) == 5);
  memory.memoryTypes[5].propertyFlags |= VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  check(mappedMemoryType(memory, UINT32_MAX) == 4);
  check(mappedMemoryType(memory, UINT32_C(1) << 5) == UINT32_MAX);
  memory.memoryTypeCount = VK_MAX_MEMORY_TYPES;
  memory.memoryTypes[31].propertyFlags = visible | coherent | cached | local;
  check(mappedMemoryType(memory, UINT32_C(1) << 31) == 31);
  memory.memoryTypeCount = VK_MAX_MEMORY_TYPES + 1;
  check(mappedMemoryType(memory, UINT32_MAX) == 31);
}
