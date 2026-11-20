// Windows implementation included inside kalandra::runtime::process.
namespace {
struct Handles {
  std::vector<HANDLE> values;
  ~Handles() {
    for (auto h : values)
      CloseHandle(h);
  }
};
int wide(const std::string &text, std::wstring &out) {
  if (text.empty()) {
    out.clear();
    return 0;
  }
  if (text.size() > INT_MAX)
    return ERROR_INVALID_PARAMETER;
  const int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
  if (!n)
    return GetLastError();
  out.resize(n);
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()),
                             out.data(), n)
             ? 0
             : int(GetLastError());
}
// Follow the Windows C runtime argv convention. Backslashes only need doubling
// before a quote or immediately before the closing quote.
void appendArg(std::wstring &line, const std::wstring &arg) {
  if (!line.empty())
    line += L' ';
  line += L'"';
  std::size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    line.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
    slashes = 0;
    line += c;
  }
  line.append(2 * slashes, L'\\');
  line += L'"';
}
int redirects(const Command &cmd, Child &child, Handles &ends, HANDLE (&standard)[3]) {
  static std::atomic<unsigned long long> serial{};
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  for (int i = 0; i < 3; ++i) {
    HANDLE h = INVALID_HANDLE_VALUE;
    if (cmd.streams[i] == Stdio::Pipe) {
      const auto name = L"\\\\.\\pipe\\kalandra-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(serial.fetch_add(1));
      const auto access = i ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND;
      const auto parent = CreateNamedPipeW(name.c_str(), access | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT |
                                               PIPE_REJECT_REMOTE_CLIENTS,
                                           1, 65536, 65536, 0, nullptr);
      if (parent == INVALID_HANDLE_VALUE)
        return GetLastError();
      child.pipes[i] = parent;
      h = CreateFileW(name.c_str(), i ? GENERIC_WRITE : GENERIC_READ, 0, &inherit, OPEN_EXISTING, 0,
                      nullptr);
    } else if (cmd.streams[i] == Stdio::Inherit) {
      HANDLE original = GetStdHandle(i == 0   ? STD_INPUT_HANDLE
                                     : i == 1 ? STD_OUTPUT_HANDLE
                                              : STD_ERROR_HANDLE);
      if (original && original != INVALID_HANDLE_VALUE &&
          !DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &h, 0, TRUE,
                           DUPLICATE_SAME_ACCESS))
        return GetLastError();
    }
    if (h == INVALID_HANDLE_VALUE && cmd.streams[i] != Stdio::Pipe)
      h = CreateFileW(L"NUL", i ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &inherit, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      return GetLastError();
    ends.values.push_back(h);
    standard[i] = h;
  }
  return 0;
}
int makeEnvironment(const Command &cmd, std::vector<wchar_t> &block) {
  std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(GetEnvironmentStringsW(),
                                                                         FreeEnvironmentStringsW);
  if (!inherited)
    return GetLastError();
  std::vector<std::wstring> entries;
  for (auto p = inherited.get(); *p; p += wcslen(p) + 1)
    entries.emplace_back(p);
  for (const auto &[key, value] : cmd.environment) {
    if (key.empty() || key.find('=') != std::string::npos)
      return ERROR_INVALID_PARAMETER;
    std::wstring name, text;
    int error;
    if ((error = wide(key, name)) || (error = wide(value, text)))
      return error;
    name += L'=';
    std::erase_if(entries, [&](const auto &s) {
      return s.size() >= name.size() &&
             CompareStringOrdinal(s.data(), int(name.size()), name.data(), int(name.size()),
                                  TRUE) == CSTR_EQUAL;
    });
    entries.push_back(name + text);
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  for (auto &s : entries) {
    block.insert(block.end(), s.begin(), s.end());
    block.push_back(0);
  }
  if (block.empty())
    block.push_back(0);
  block.push_back(0);
  return 0;
}
} // namespace
int launch(const Command &cmd, Child &child) {
  if (cmd.program.empty())
    return ERROR_INVALID_PARAMETER;
  std::wstring program, line, directory;
  int error = wide(cmd.program, program);
  if (error || (error = wide(cmd.directory, directory)))
    return error;
  appendArg(line, program);
  for (const auto &arg : cmd.arguments) {
    std::wstring text;
    if ((error = wide(arg, text)))
      return error;
    appendArg(line, text);
  }
  std::vector<wchar_t> environment;
  if (!cmd.environment.empty() && (error = makeEnvironment(cmd, environment)))
    return error;
  Handles ends;
  ends.values.reserve(3);
  HANDLE standard[3];
  if ((error = redirects(cmd, child, ends, standard)))
    return error;
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = standard[0];
  si.StartupInfo.hStdOutput = standard[1];
  si.StartupInfo.hStdError = standard[2];
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes))
    return GetLastError();
  PROCESS_INFORMATION pi{};
  if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, standard,
                                 sizeof(standard), nullptr, nullptr) ||
      !CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                      environment.empty() ? nullptr : environment.data(),
                      directory.empty() ? nullptr : directory.c_str(), &si.StartupInfo, &pi))
    error = GetLastError();
  DeleteProcThreadAttributeList(si.lpAttributeList);
  if (error)
    return error;
  child.process = pi.hProcess;
  CloseHandle(pi.hThread);
  return 0;
}
int poll(Child &child) noexcept {
  if (child.finished)
    return 0;
  auto result = WaitForSingleObject(child.process, 0);
  if (result == WAIT_TIMEOUT)
    return 0;
  DWORD code;
  if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(child.process, &code))
    return GetLastError();
  child.finished = true;
  child.status.value = code;
  return 0;
}
Child::~Child() {
  for (auto file : pipes)
    if (file != invalidFile)
      CloseHandle(file);
  if (process) {
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      TerminateProcess(process, 1);
      WaitForSingleObject(process, INFINITE);
    }
    CloseHandle(process);
  }
}
