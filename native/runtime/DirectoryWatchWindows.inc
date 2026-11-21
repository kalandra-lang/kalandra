struct WatchHandle {
  HANDLE value;
  explicit WatchHandle(HANDLE h) : value(h) {
    if (!h || h == INVALID_HANDLE_VALUE)
      fail(GetLastError());
  }
  ~WatchHandle() { CloseHandle(value); }
  WatchHandle(const WatchHandle &) = delete;
};
class Backend {
  WatchHandle dir_, event_;
  OVERLAPPED request_{};
  alignas(DWORD) std::array<unsigned char, 32768> bytes_{};
  bool recursive_, pending_ = false;
  Events &events_;
  void arm() {
    request_ = {};
    request_.hEvent = event_.value;
    ResetEvent(event_.value);
    constexpr DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                            FILE_NOTIFY_CHANGE_LAST_WRITE;
    if (!ReadDirectoryChangesW(dir_.value, bytes_.data(), DWORD(bytes_.size()), recursive_, flags,
                               nullptr, &request_, nullptr))
      fail(GetLastError());
    pending_ = true;
  }

public:
  Backend(const fs::path &root, bool recursive, Events &events, const std::atomic<bool> &)
      : dir_(CreateFileW(
            root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr)),
        event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)), recursive_(recursive),
        events_(events) {
    arm();
  }
  ~Backend() {
    if (pending_) {
      // The buffer and OVERLAPPED must outlive completion, including cancellation.
      CancelIoEx(dir_.value, &request_);
      DWORD ignored;
      GetOverlappedResult(dir_.value, &request_, &ignored, TRUE);
    }
  }
  void poll(const std::atomic<bool> &stop) {
    DWORD n = 0;
    if (!GetOverlappedResult(dir_.value, &request_, &n, FALSE)) {
      const auto error = GetLastError();
      if (error == ERROR_IO_INCOMPLETE)
        return;
      pending_ = false;
      if (error != ERROR_NOTIFY_ENUM_DIR)
        fail(error);
      n = 0;
    }
    pending_ = false;
    if (!n)
      events_.rescan();
    else {
      for (DWORD i = 0; i < n && !stop;) {
        auto *entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(bytes_.data() + i);
        events_.changed(
            std::wstring_view(entry->FileName, entry->FileNameLength / sizeof(wchar_t)));
        if (!entry->NextEntryOffset)
          break;
        i += entry->NextEntryOffset;
      }
    }
    arm();
  }
};
