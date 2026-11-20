#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kalandra::runtime::process {
#ifdef _WIN32
using File = HANDLE;
inline const File invalidFile = INVALID_HANDLE_VALUE;
#else
using File = int;
inline constexpr File invalidFile = -1;
#endif
enum class Stdio { Inherit, Pipe, Null };
struct Command {
  std::string program, directory;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;
  std::array<Stdio, 3> streams{};
};
struct Status {
  std::uint32_t value = 0;
  bool signaled = false;
};
struct Child {
#ifdef _WIN32
  HANDLE process = nullptr;
#else
  int pid = -1;
#endif
  std::array<File, 3> pipes{invalidFile, invalidFile, invalidFile};
  Status status;
  bool finished = false;
  Child() = default;
  Child(const Child &) = delete;
  Child &operator=(const Child &) = delete;
  ~Child();
};
// A fresh Child owns every endpoint and the process even on partial failure.
int launch(const Command &, Child &);
int poll(Child &) noexcept;
} // namespace kalandra::runtime::process
