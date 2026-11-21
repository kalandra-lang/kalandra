struct NotifyFd {
  int value = -1;
  ~NotifyFd() {
    if (value >= 0)
      ::close(value);
  }
  NotifyFd() = default;
  NotifyFd(const NotifyFd &) = delete;
};
class Backend {
  NotifyFd fd_;
  fs::path root_;
  bool recursive_;
  Events &events_;
  std::unordered_map<int, fs::path> directories_;
  static constexpr auto mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                               IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_DELETE_SELF |
                               IN_ONLYDIR | IN_DONT_FOLLOW;

  bool add(const fs::path &path, const fs::path &relative, bool root = false) {
    const auto wd = inotify_add_watch(fd_.value, path.c_str(), mask);
    if (wd < 0) {
      if (!root && (errno == ENOENT || errno == ENOTDIR))
        return false;
      fail(errno);
    }
    if (directories_.size() >= 65536)
      fail(ENOSPC);
    return directories_.emplace(wd, relative).second;
  }
  void arm(const std::atomic<bool> &stop) {
    // A fresh descriptor discards obsolete watch IDs and stale rename records.
    // Register each directory before enumerating its children.
    NotifyFd fresh;
    fresh.value = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fresh.value < 0)
      fail(errno);
    std::swap(fd_.value, fresh.value);
    directories_.clear();
    add(root_, {}, true);
    if (!recursive_)
      return;
    for (fs::recursive_directory_iterator it(root_), end; it != end && !stop; ++it) {
      if (fs::is_directory(it->symlink_status()) &&
          !add(it->path(), it->path().lexically_relative(root_)))
        it.disable_recursion_pending();
    }
  }

public:
  Backend(const fs::path &root, bool recursive, Events &events, const std::atomic<bool> &stop)
      : root_(root), recursive_(recursive), events_(events) {
    arm(stop);
  }
  void poll(const std::atomic<bool> &stop) {
    alignas(inotify_event) std::array<char, 32768> bytes;
    const auto n = ::read(fd_.value, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR)
        return;
      fail(errno);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(n) && !stop;) {
      const auto *record = reinterpret_cast<const inotify_event *>(bytes.data() + i);
      i += sizeof(*record) + record->len;
      auto found = directories_.find(record->wd);
      if (found != directories_.end() && found->second.empty() &&
          (record->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_UNMOUNT)))
        fail(ESTALE);
      if ((record->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_UNMOUNT)) ||
          (recursive_ && (record->mask & IN_ISDIR) &&
           (record->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)))) {
        // Children could already have changed before their registration.
        arm(stop);
        events_.rescan();
        return;
      }
      if (found != directories_.end())
        events_.changed(record->len ? found->second / record->name : found->second);
    }
  }
};
