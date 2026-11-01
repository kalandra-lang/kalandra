#include "VulkanLoader.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace kalandra::runtime::vulkan {
namespace {
class Loader final {
public:
  VulkanApi api;
  Loader() noexcept {
#ifdef _WIN32
    // Never search the executable/current directory for the system GPU loader.
    module_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    module_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!module_)
      return;
    bool complete = true;
#define KALANDRA_VULKAN_FUNCTION(name) \
    api.name = reinterpret_cast<PFN_##name>(symbol(#name)); \
    complete = complete && api.name != nullptr;
#include "VulkanFunctions.inc"
#undef KALANDRA_VULKAN_FUNCTION
    if (!complete) {
      api = {};
      close();
      return;
    }
    api.available = true;
  }
  // A successful OS loader handle is process lifetime, like an ordinary loader
  // import. This keeps dispatch pointers valid even for GPU captures released
  // by unrelated static TaskCore owners during shutdown. Vulkan device/instance
  // and ICD lifetimes are still retired normally; failed publication closes it.

  Loader(const Loader &) = delete;
  Loader &operator=(const Loader &) = delete;
private:
#ifdef _WIN32
  HMODULE module_ = nullptr;
  FARPROC symbol(const char *name) noexcept { return GetProcAddress(module_, name); }
#else
  void *module_ = nullptr;
  void *symbol(const char *name) noexcept { return dlsym(module_, name); }
#endif
  void close() noexcept {
    if (!module_)
      return;
#ifdef _WIN32
    FreeLibrary(module_);
#else
    dlclose(module_);
#endif
    module_ = nullptr;
  }
};
} // namespace
const VulkanApi &vulkanApi() noexcept {
  // Thread-safe publication, including concurrent first Runtime creation.
  static const Loader loader;
  return loader.api;
}
} // namespace kalandra::runtime::vulkan
