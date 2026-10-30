#pragma once
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <random>
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace kalandra::detail {
inline std::error_code replaceFile(const std::filesystem::path &source,
                                   const std::filesystem::path &destination) {
#ifdef _WIN32
  return MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? std::error_code{}
             : std::error_code(int(GetLastError()), std::system_category());
#else
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  return error;
#endif
}
inline std::error_code createPrivateDirectory(const std::filesystem::path &path) {
#ifdef _WIN32
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor,
          nullptr))
    return {int(GetLastError()), std::system_category()};
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  const bool created = CreateDirectoryW(path.c_str(), &attributes);
  const auto error = created ? 0 : GetLastError();
  LocalFree(descriptor);
  return {int(error), std::system_category()};
#else
  return ::mkdir(path.c_str(), 0700) ? std::error_code(errno, std::generic_category())
                                     : std::error_code{};
#endif
}

inline std::error_code createPrivateStage(const std::filesystem::path &parent,
                                          const std::string &prefix,
                                          std::filesystem::path &result) {
#ifdef _WIN32
  std::random_device random;
  for (unsigned attempt = 0; attempt != 32; ++attempt) {
    auto name = prefix;
    for (unsigned block = 0; block != 4; ++block) {
      auto bits = random();
      for (unsigned digit = 0; digit != 8; ++digit, bits >>= 4)
        name += "0123456789abcdef"[bits & 15];
    }
    result = parent / name;
    auto error = createPrivateDirectory(result);
    if (error != std::errc::file_exists)
      return error;
  }
  return std::make_error_code(std::errc::file_exists);
#else
  auto pattern = (parent / (prefix + "XXXXXX")).string();
  if (!::mkdtemp(pattern.data()))
    return {errno, std::generic_category()};
  result = pattern;
  return {};
#endif
}

inline std::error_code publishDirectoryExclusive(const std::filesystem::path &source,
                                                 const std::filesystem::path &destination) {
#ifdef _WIN32
  return MoveFileExW(source.c_str(), destination.c_str(), 0)
             ? std::error_code{}
             : std::error_code(int(GetLastError()), std::system_category());
#elif defined(__APPLE__)
  return ::renameatx_np(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCL)
             ? std::error_code(errno, std::generic_category())
             : std::error_code{};
#elif defined(__linux__)
  return ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                   RENAME_NOREPLACE)
             ? std::error_code(errno, std::generic_category())
             : std::error_code{};
#else
  return std::make_error_code(std::errc::operation_not_supported);
#endif
}
} // namespace kalandra::detail
