// Independent sanitizer control: uses Vulkan directly, no Kalandra runtime.
#include <cstdio>
#include <vector>
#include <vulkan/vulkan.h>
int main() {
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
  vkDestroyInstance(instance, nullptr);
  std::printf("Loader-only control: %u devices, status %d\n", count, int(status));
  return status == VK_SUCCESS ? 0 : 1;
}
