// Included inside FileSystem.cpp's private namespace: utilities share the
// bounded executor, request ledger and deferred-close slots with file I/O.
#pragma once
#ifdef _WIN32
bool windowsPath(Job &job, wchar_t *path, int capacity) noexcept {
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char *>(job.data()),
                          -1, path, capacity))
    return true;
  job.system(lastError());
  return false;
}
std::uint64_t nodeKind(DWORD attributes, DWORD tag) noexcept {
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT))
    return 3;
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
    return 4;
  return attributes & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
}
#else
std::uint64_t nodeKind(mode_t mode) noexcept {
  return S_ISREG(mode) ? 1 : S_ISDIR(mode) ? 2 : S_ISLNK(mode) ? 3 : 4;
}
#endif
void openDirectory(Job &job) noexcept {
  const auto slot = reserveFileSlot();
  if (slot == noFileSlot) {
    job.kind = FileKind::ResourceLimit;
    return;
  }
  auto &file = *job.output;
  file.closeSlot = slot;
  file.isDirectory = true;
  auto *data = new (file.data()) DirectoryData;
#ifdef _WIN32
  std::array<wchar_t, 32768> path;
  if (!windowsPath(job, path.data(), int(path.size())))
    return;
  const auto attributes = GetFileAttributesW(path.data());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    job.system(lastError());
    return;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    job.system(ERROR_DIRECTORY);
    return;
  }
  auto length = std::wcslen(path.data());
  if (length + 3 > path.size()) {
    job.system(ERROR_FILENAME_EXCED_RANGE);
    return;
  }
  if (length && path[length - 1] != L'\\' && path[length - 1] != L'/')
    path[length++] = L'\\';
  path[length++] = L'*';
  path[length] = 0;
  file.directory = FindFirstFileExW(path.data(), FindExInfoBasic, &data->first,
                                    FindExSearchNameMatch, nullptr, 0);
  if (file.directory == invalidDirectory) {
    const auto error = lastError();
    if (error == ERROR_FILE_NOT_FOUND)
      data->exhausted = true;
    else
      job.system(error);
  } else
    data->firstPending = true;
#else
  (void)data;
  do {
    file.directory = ::opendir(reinterpret_cast<const char *>(job.data()));
  } while (!file.directory && errno == EINTR && !job.abandoned);
  if (!file.directory)
    job.system(lastError());
#endif
}
void readDirectory(Job &job, FileState &file) noexcept {
  auto &data = *reinterpret_cast<DirectoryData *>(file.data());
  if (data.exhausted) {
    job.kind = FileKind::End;
    return;
  }
  while (!job.abandoned.load(std::memory_order_acquire)) {
#ifdef _WIN32
    WIN32_FIND_DATAW entry;
    if (data.firstPending) {
      entry = data.first;
      data.firstPending = false;
    } else if (!FindNextFileW(file.directory, &entry)) {
      const auto error = lastError();
      if (error == ERROR_NO_MORE_FILES) {
        data.exhausted = true;
        job.kind = FileKind::End;
      } else
        job.system(error);
      return;
    }
    if (!std::wcscmp(entry.cFileName, L".") || !std::wcscmp(entry.cFileName, L".."))
      continue;
    const auto length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1,
                            reinterpret_cast<char *>(job.data()), int(job.size), nullptr, nullptr);
    if (!length) {
      const auto error = lastError();
      if (error == ERROR_NO_UNICODE_TRANSLATION)
        job.kind = FileKind::InvalidEncoding;
      else
        job.system(error);
      return;
    }
    job.count = std::uint64_t(length - 1);
    job.mode = nodeKind(entry.dwFileAttributes, entry.dwReserved0);
#else
    errno = 0;
    auto *entry = ::readdir(file.directory);
    if (!entry) {
      if (errno)
        job.system(lastError());
      else {
        data.exhausted = true;
        job.kind = FileKind::End;
      }
      return;
    }
    if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
      continue;
    const auto length = std::strlen(entry->d_name);
    if (length >= job.size) {
      job.system(ENAMETOOLONG);
      return;
    }
    const std::string_view name(entry->d_name, length);
    if (kalandra::validateUtf8(name, [&] {
          return !job.abandoned.load(std::memory_order_acquire);
        }) != kalandra::Utf8Status::Valid) {
      if (!job.abandoned)
        job.kind = FileKind::InvalidEncoding;
      return;
    }
    std::memcpy(job.data(), name.data(), name.size());
    job.count = length;
    switch (entry->d_type) {
    case DT_REG:
      job.mode = 1;
      break;
    case DT_DIR:
      job.mode = 2;
      break;
    case DT_LNK:
      job.mode = 3;
      break;
    case DT_UNKNOWN: {
      struct stat info;
      if (::fstatat(::dirfd(file.directory), entry->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
        job.system(lastError());
        return;
      }
      job.mode = nodeKind(info.st_mode);
      break;
    }
    default:
      job.mode = 4;
      break;
    }
#endif
    return;
  }
}
void pathUtility(Job &job) noexcept {
#ifdef _WIN32
  std::array<wchar_t, 32768> path;
  if (!windowsPath(job, path.data(), int(path.size())))
    return;
  if (job.action == FileAction::Metadata) {
    const auto handle = CreateFileW(
        path.data(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == invalidFile) {
      job.system(lastError());
      return;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)))
      job.system(lastError());
    else {
      job.mode = nodeKind(attributes.FileAttributes, attributes.ReparseTag);
      if (job.mode == 1)
        job.count = std::uint64_t(info.EndOfFile.QuadPart);
    }
    (void)CloseHandle(handle);
    return;
  }
  const bool success = job.action == FileAction::DirectoryCreate
                           ? CreateDirectoryW(path.data(), nullptr)
                       : job.action == FileAction::DirectoryRemove ? RemoveDirectoryW(path.data())
                                                                   : DeleteFileW(path.data());
  if (!success)
    job.system(lastError());
#else
  auto *path = reinterpret_cast<char *>(job.data());
  if (job.action == FileAction::Metadata) {
    // A trailing slash otherwise asks POSIX pathname resolution to follow the
    // final symlink before lstat sees it. Keep root, but remove that request.
    auto size = std::strlen(path);
    while (size > 1 && path[size - 1] == '/')
      path[--size] = 0;
    struct stat info;
    if (::lstat(path, &info)) {
      job.system(lastError());
      return;
    }
    job.mode = nodeKind(info.st_mode);
    if (job.mode == 1) {
      if (info.st_size < 0)
        job.system(EOVERFLOW);
      else
        job.count = std::uint64_t(info.st_size);
    }
    return;
  }
  const auto result = job.action == FileAction::DirectoryCreate   ? ::mkdir(path, 0777)
                      : job.action == FileAction::DirectoryRemove ? ::rmdir(path)
                                                                  : ::unlink(path);
  if (result)
    job.system(lastError());
#endif
}
