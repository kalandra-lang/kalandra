// Open a single component relative to an existing directory object. A full
// path lookup here would re-resolve ancestors between enumeration and deletion.
struct TreeHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~TreeHandle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  TreeHandle() = default;
  TreeHandle(const TreeHandle &) = delete;
  TreeHandle &operator=(const TreeHandle &) = delete;
};
DWORD treeAttributes(HANDLE handle, DWORD &tag) {
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
    treeSystemError(lastError());
  tag = info.ReparseTag;
  return info.FileAttributes;
}
HANDLE treeRelative(HANDLE base, const fs::path &name, DWORD access) {
  static const auto module = GetModuleHandleW(L"ntdll.dll");
  static const auto create =
      reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(module, "NtCreateFile"));
  static const auto toError =
      reinterpret_cast<ULONG(WINAPI *)(NTSTATUS)>(GetProcAddress(module, "RtlNtStatusToDosError"));
  if (!create || !toError)
    treeSystemError(ERROR_PROC_NOT_FOUND);
  const auto &part = name.native();
  if (part.empty() || part.size() > USHRT_MAX / sizeof(wchar_t) ||
      part.find_first_of(L"\\/:") != part.npos || part == L"." || part == L"..")
    throw InvalidTreePath{};
  UNICODE_STRING text{};
  text.Length = text.MaximumLength = static_cast<USHORT>(part.size() * sizeof(wchar_t));
  text.Buffer = const_cast<wchar_t *>(part.c_str());
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = base;
  attributes.ObjectName = &text;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK io{};
  HANDLE result = nullptr;
  NTSTATUS status =
      create(&result, SYNCHRONIZE | FILE_READ_ATTRIBUTES | access, &attributes, &io, nullptr, 0,
             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
             FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, nullptr, 0);
  if (status < 0)
    treeSystemError(toError(status));
  return result;
}
struct TreeDirectory {
  TreeSlot slot;
  TreeHandle handle;
  alignas(8) std::array<std::byte, 4096> buffer{};
  DWORD offset = 0;
  bool refill = true;
  bool next(fs::path &name, std::uint64_t &kind) {
    for (;;) {
      if (refill) {
        if (!GetFileInformationByHandleEx(handle.value, FileFullDirectoryInfo, buffer.data(),
                                          DWORD(buffer.size()))) {
          const auto error = lastError();
          if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND)
            return false;
          treeSystemError(error);
        }
        offset = 0;
      }
      auto *entry = reinterpret_cast<const FILE_FULL_DIR_INFO *>(buffer.data() + offset);
      refill = entry->NextEntryOffset == 0;
      offset += entry->NextEntryOffset;
      name = std::wstring_view(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name == L"." || name == L"..")
        continue;
      const auto flags = entry->FileAttributes;
      if (flags & FILE_ATTRIBUTE_REPARSE_POINT) {
        TreeHandle leaf;
        leaf.value = treeRelative(handle.value, name, 0);
        DWORD tag;
        treeAttributes(leaf.value, tag);
        kind = tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT ? 3 : 4;
      } else
        kind = flags & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
      return true;
    }
  }
};
std::unique_ptr<TreeDirectory> treeOpen(const TreeDirectory *parent, const fs::path &name) {
  auto frame = std::make_unique<TreeDirectory>();
  const auto access = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
  frame->handle.value =
      parent ? treeRelative(parent->handle.value, name, access)
             : CreateFileW(name.c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
  if (frame->handle.value == INVALID_HANDLE_VALUE)
    treeSystemError(lastError());
  DWORD tag;
  const auto flags = treeAttributes(frame->handle.value, tag);
  if (!(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
    treeSystemError(ERROR_DIRECTORY);
  return frame;
}
void deleteOpenedTreeEntry(HANDLE handle) {
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)))
    treeSystemError(lastError());
}
std::unique_ptr<TreeDirectory> treeDeleteEntry(TreeDirectory &parent, const fs::path &name,
                                               bool root, bool directoryHint) {
  auto frame = std::make_unique<TreeDirectory>();
  frame->handle.value =
      treeRelative(parent.handle.value, name,
                   DELETE | (directoryHint ? FILE_LIST_DIRECTORY | FILE_TRAVERSE : 0));
  DWORD tag;
  const auto flags = treeAttributes(frame->handle.value, tag);
  if ((flags & FILE_ATTRIBUTE_DIRECTORY) && !(flags & FILE_ATTRIBUTE_REPARSE_POINT)) {
    if (!directoryHint)
      treeSystemError(ERROR_DIRECTORY);
    return frame;
  }
  if (root && !(flags & FILE_ATTRIBUTE_REPARSE_POINT))
    throw InvalidTreePath{};
  deleteOpenedTreeEntry(frame->handle.value);
  return {};
}
void treeFinishDelete(TreeDirectory &, const fs::path &, TreeDirectory &dir) {
  deleteOpenedTreeEntry(dir.handle.value);
}
