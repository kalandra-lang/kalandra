#include "VulkanLoader.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <cstdio>
#include <string>
#include <windows.h>
int main() {
  const auto &api = kalandra::runtime::vulkan::vulkanApi();
  if (api.available) {
    const auto module = GetModuleHandleW(L"vulkan-1.dll");
    std::array<wchar_t, 32768> loaded{}, system{};
    const auto count = GetModuleFileNameW(module, loaded.data(), DWORD(loaded.size()));
    const auto length = GetSystemDirectoryW(system.data(), UINT(system.size()));
    if (!module || !count || count >= loaded.size() || !length || length >= system.size())
      return 1;
    const std::wstring expected = std::wstring(system.data(), length) + L"\\vulkan-1.dll";
    if (CompareStringOrdinal(loaded.data(), int(count), expected.data(), int(expected.size()),
                             TRUE) != CSTR_EQUAL)
      return 1;
  }
  std::printf(
      "PASS: Windows system-only Vulkan lookup; complete table available=%u; no GPU execution\n",
      unsigned(api.available));
}
