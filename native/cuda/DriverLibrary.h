#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace kalandra::runtime::cuda {
// Driver-only OS lookup. No CUDA runtime import, SDK DLL, current-directory
// search or GPU initialization. The owning State outlives all captured work.
class DriverLibrary final {
#ifdef _WIN32
  HMODULE handle_ = nullptr;
#else
  void *handle_ = nullptr;
#endif
public:
  DriverLibrary() = default;
  DriverLibrary(const DriverLibrary &) = delete;
  DriverLibrary &operator=(const DriverLibrary &) = delete;
  ~DriverLibrary() {
    if (!handle_)
      return;
#ifdef _WIN32
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }
  bool open() noexcept {
    if (handle_)
      return true;
#ifdef _WIN32
    handle_ = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    handle_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
    return handle_ != nullptr;
  }
  void *symbol(const char *name) const noexcept {
    if (!handle_)
      return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(handle_, name));
#else
    return dlsym(handle_, name);
#endif
  }
#ifdef _WIN32
  HMODULE nativeHandle() const noexcept { return handle_; }
#endif
};
} // namespace kalandra::runtime::cuda
