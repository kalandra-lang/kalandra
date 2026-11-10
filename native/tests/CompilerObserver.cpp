// Test-only launcher: numerical fixtures explicitly select the existing
// result-observation interface. Never installed or used by application tests.
#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
int wmain(int argc, wchar_t **argv) {
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, KALANDRA_COMPILER_PATH, -1, nullptr, 0);
  if (!size)
    return 2;
  std::wstring compiler(size, L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, KALANDRA_COMPILER_PATH, -1,
                           compiler.data(), size))
    return 2;
  compiler.pop_back();
  std::vector<std::wstring> values{compiler};
  bool selected = false, compiling = false;
  for (int i = 1; i < argc; ++i) {
    selected |= std::wstring_view(argv[i]) == L"--test-entry";
    compiling |= std::wstring_view(argv[i]) == L"-o";
    values.emplace_back(argv[i]);
  }
  if (compiling && !selected) {
    values.emplace_back(L"--test-entry");
    values.emplace_back(L"main");
  }
  std::vector<const wchar_t *> arguments;
  for (const auto &value : values)
    arguments.push_back(value.c_str());
  arguments.push_back(nullptr);
  const auto result = _wspawnv(_P_WAIT, compiler.c_str(), arguments.data());
  if (result != -1)
    return static_cast<int>(result);
  std::perror("cannot run numerical test compiler");
  return 2;
}
#else
#include <unistd.h>
int main(int argc, char **argv) {
  std::vector<std::string> values{KALANDRA_COMPILER_PATH};
  bool selected = false, compiling = false;
  for (int i = 1; i < argc; ++i) {
    selected |= std::string_view(argv[i]) == "--test-entry";
    compiling |= std::string_view(argv[i]) == "-o";
    values.emplace_back(argv[i]);
  }
  if (compiling && !selected) {
    values.emplace_back("--test-entry");
    values.emplace_back("main");
  }
  std::vector<char *> arguments;
  for (auto &value : values)
    arguments.push_back(value.data());
  arguments.push_back(nullptr);
  ::execv(KALANDRA_COMPILER_PATH, arguments.data());
  std::perror("cannot run numerical test compiler");
  return 2;
}
#endif
