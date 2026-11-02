#include "DriverLibrary.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::cuda::DriverLibrary;
int main() {
  try {
    DriverLibrary library;
    if (library.symbol("cuInit"))
      throw std::runtime_error("unopened driver publication");
    if (library.open()) {
      wchar_t loaded[32768]{}, system[32768]{};
      auto length = GetModuleFileNameW(library.nativeHandle(), loaded, 32768);
      auto count = GetSystemDirectoryW(system, 32768);
      if (!length || length >= 32768 || !count || count >= 32768)
        throw std::runtime_error("driver path query");
      auto normalize = [](std::filesystem::path p) {
        auto value = p.lexically_normal().wstring();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c) { return wchar_t(std::towlower(c)); });
        return value;
      };
      if (normalize(loaded) != normalize(std::filesystem::path(system) / L"nvcuda.dll"))
        throw std::runtime_error("CUDA driver loaded outside System32");
      if (!library.symbol("cuInit"))
        throw std::runtime_error("system driver interface");
    }
    std::cout << "PASS: CUDA System32-only loader; local decoy rejected; no GPU initialization\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
