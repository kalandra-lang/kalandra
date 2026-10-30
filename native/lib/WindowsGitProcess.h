#pragma once
#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace kalandra::detail {
inline void windowsRequire(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
inline std::wstring windowsWide(const std::string &text) {
  const auto length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
  windowsRequire(length > 0 || text.empty(), "Git argument is not valid UTF-8");
  std::wstring result(length, L'\0');
  if (length)
    windowsRequire(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()),
                                       result.data(), length) == length,
                   "Cannot convert Git argument to UTF-16");
  return result;
}
inline std::wstring windowsEnvironment(const wchar_t *name) {
  const auto length = GetEnvironmentVariableW(name, nullptr, 0);
  if (!length)
    return {};
  std::wstring value(length, L'\0');
  const auto count = GetEnvironmentVariableW(name, value.data(), length);
  windowsRequire(count > 0 && count < length, "Cannot read Windows platform environment");
  value.resize(count);
  return value;
}
inline std::filesystem::path windowsGit() {
  auto override = windowsEnvironment(L"KALANDRA_GIT");
  const auto programFiles = windowsEnvironment(L"ProgramW6432");
  const auto path = override.empty() ? std::filesystem::path(programFiles) / "Git/cmd/git.exe"
                                     : std::filesystem::path(override);
  windowsRequire(path.is_absolute() &&
                     std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
                 "Install Git for Windows or set KALANDRA_GIT to its absolute git.exe path");
  return std::filesystem::canonical(path);
}
// Microsoft C runtime command-line quoting: preserve every argument, including
// trailing backslashes and quotes. No shell or PATH-based executable lookup.
inline std::wstring windowsQuote(const std::wstring &text) {
  std::wstring result = L"\"";
  unsigned backslashes = 0;
  for (auto c : text) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    result.append(backslashes * (c == L'"' ? 2 : 1), L'\\');
    backslashes = 0;
    if (c == L'"')
      result += L'\\';
    result += c;
  }
  result.append(backslashes * 2, L'\\');
  return result + L'"';
}
struct WindowsGitChild {
  HANDLE pipe = nullptr, writer = nullptr, null = INVALID_HANDLE_VALUE;
  HANDLE job = nullptr, process = nullptr, thread = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
  bool attributesInitialized = false;
  ~WindowsGitChild() {
    if (job)
      TerminateJobObject(job, 1);
    if (process) {
      // Covers failure before the suspended process was attached to the job.
      TerminateProcess(process, 1);
      WaitForSingleObject(process, INFINITE);
    }
    for (auto handle : {pipe, writer, null, thread, process, job})
      if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    if (attributesInitialized)
      DeleteProcThreadAttributeList(attributes);
    if (attributes)
      HeapFree(GetProcessHeap(), 0, attributes);
  }
};
template <class Budget>
std::string windowsGitProcess(const std::filesystem::path &executable,
                              const std::vector<std::string> &arguments,
                              const std::filesystem::path &directory, std::size_t maximum,
                              unsigned seconds, Budget checkBudget) {
  WindowsGitChild child;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  windowsRequire(CreatePipe(&child.pipe, &child.writer, &security, 0),
                 "Cannot create Git output pipe");
  windowsRequire(SetHandleInformation(child.pipe, HANDLE_FLAG_INHERIT, 0),
                 "Cannot isolate Git output pipe");
  child.null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &security, OPEN_EXISTING, 0, nullptr);
  windowsRequire(child.null != INVALID_HANDLE_VALUE, "Cannot isolate Git input");
  child.job = CreateJobObjectW(nullptr, nullptr);
  windowsRequire(child.job != nullptr, "Cannot create Git process job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  limits.BasicLimitInformation.ActiveProcessLimit = 16;
  windowsRequire(SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits,
                                         sizeof(limits)),
                 "Cannot bound Git child processes");
  SIZE_T attributeBytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
  child.attributes =
      static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attributeBytes));
  windowsRequire(child.attributes != nullptr, "Cannot allocate Git process attributes");
  windowsRequire(InitializeProcThreadAttributeList(child.attributes, 1, 0, &attributeBytes),
                 "Cannot isolate Git inherited handles");
  child.attributesInitialized = true;
  HANDLE inherited[] = {child.writer, child.null};
  windowsRequire(UpdateProcThreadAttribute(child.attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited, sizeof(inherited), nullptr, nullptr),
                 "Cannot isolate Git inherited handles");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child.null;
  startup.StartupInfo.hStdOutput = child.writer;
  startup.StartupInfo.hStdError = child.null;
  startup.lpAttributeList = child.attributes;
  wchar_t system[MAX_PATH];
  const auto systemLength = GetWindowsDirectoryW(system, MAX_PATH);
  windowsRequire(systemLength && systemLength < MAX_PATH,
                 "Cannot locate Windows platform directory");
  const std::filesystem::path systemRoot(system);
  const auto gitRoot = executable.parent_path().parent_path();
  std::map<std::wstring, std::wstring> environment{
      {L"GIT_CONFIG_NOSYSTEM", L"1"},
      {L"GIT_CONFIG_SYSTEM", L"NUL"},
      {L"GIT_CONFIG_GLOBAL", L"NUL"},
      {L"GIT_TERMINAL_PROMPT", L"0"},
      {L"GIT_ASKPASS", L"NUL"},
      {L"GIT_ATTR_NOSYSTEM", L"1"},
      {L"GIT_NO_LAZY_FETCH", L"1"},
      {L"GIT_NO_REPLACE_OBJECTS", L"1"},
      {L"GIT_OPTIONAL_LOCKS", L"0"},
      {L"HOME", directory.wstring()},
      {L"USERPROFILE", directory.wstring()},
      {L"LC_ALL", L"C"},
      {L"SystemRoot", systemRoot.wstring()},
      {L"TEMP", directory.wstring()},
      {L"TMP", directory.wstring()},
      {L"PATH", (gitRoot / "mingw64/bin").wstring() + L";" + (gitRoot / "ucrt64/bin").wstring() +
                    L";" + (gitRoot / "usr/bin").wstring() + L";" +
                    (systemRoot / "System32").wstring()}};
  std::wstring block;
  for (const auto &[key, value] : environment) {
    block += key + L"=" + value;
    block += L'\0';
  }
  block += L'\0';
  auto command = windowsQuote(executable.wstring());
  for (std::size_t index = 1; index < arguments.size(); ++index)
    command += L" " + windowsQuote(windowsWide(arguments[index]));
  PROCESS_INFORMATION process{};
  windowsRequire(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                    EXTENDED_STARTUPINFO_PRESENT,
                                block.data(), directory.c_str(), &startup.StartupInfo, &process),
                 "Cannot start Git plumbing process");
  child.process = process.hProcess;
  child.thread = process.hThread;
  windowsRequire(AssignProcessToJobObject(child.job, child.process),
                 "Cannot isolate Git process tree");
  windowsRequire(ResumeThread(child.thread) != DWORD(-1), "Cannot resume Git plumbing process");
  CloseHandle(child.writer);
  child.writer = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::string result;
  bool eof = false, exited = false;
  while (!eof || !exited) {
    windowsRequire(std::chrono::steady_clock::now() < deadline,
                   "Git plumbing exceeded configured timeout");
    checkBudget();
    DWORD available = 0;
    if (!eof && !PeekNamedPipe(child.pipe, nullptr, 0, nullptr, &available, nullptr)) {
      windowsRequire(GetLastError() == ERROR_BROKEN_PIPE, "Cannot inspect Git output");
      eof = true;
    }
    if (available) {
      char buffer[16384];
      DWORD count = 0;
      windowsRequire(
          ReadFile(child.pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr),
          "Cannot read Git output");
      windowsRequire(count <= maximum - result.size(), "Git output exceeds configured byte limit");
      result.append(buffer, count);
    }
    const auto waited = WaitForSingleObject(child.process, available ? 0 : 10);
    windowsRequire(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT,
                   "Cannot wait for Git plumbing");
    exited = waited == WAIT_OBJECT_0;
    if (exited && !available && !eof)
      Sleep(1);
  }
  DWORD status = 0;
  windowsRequire(GetExitCodeProcess(child.process, &status) && status == 0,
                 "Git plumbing failed (network, TLS, authentication, object budget, or unavailable "
                 "pinned commit)");
  checkBudget();
  return result;
}
} // namespace kalandra::detail
#endif
