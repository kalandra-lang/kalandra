// Independent sanitizer control: uses Vulkan directly, no Kalandra runtime.
#include "SanitizerMaps.h"
#include <cstdio>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
int control(bool maps) {
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &application;
  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
    return 1;
  unsigned count = 0;
  auto status = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  if (status == VK_SUCCESS)
    status = vkEnumeratePhysicalDevices(instance, &count, devices.data());
  // Preserve addresses before driver unload so sanitizer frames reported as
  // <unknown module> at process exit can be attributed without suppressions.
  const bool mapped = !maps || writeSanitizerMaps();
  vkDestroyInstance(instance, nullptr);
  std::printf("Loader-only control: %u devices, status %d\n", count, int(status));
  return !mapped ? 2 : status == VK_SUCCESS ? 0 : 1;
}
int main(int argc, char **argv) {
  const bool twice = argc == 2 && std::string_view(argv[1]) == "--maps-twice";
  if (argc > 2 || (argc == 2 && !twice && std::string_view(argv[1]) != "--maps"))
    return 2;
  for (unsigned i = 0; i < (twice ? 2u : 1u); ++i)
    if (auto result = control(argc == 2))
      return result;
  return 0;
}
