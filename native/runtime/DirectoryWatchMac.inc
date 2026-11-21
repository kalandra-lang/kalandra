// Load the system frameworks when opening the first watcher. Cross-linked CPU
// programs retain the same libSystem/libc++ link interface as native programs.
class Framework {
  void *library_;

public:
  explicit Framework(const char *path) : library_(dlopen(path, RTLD_LOCAL | RTLD_LAZY)) {
    if (!library_)
      fail(ENOTSUP);
  }
  ~Framework() { dlclose(library_); }
  Framework(const Framework &) = delete;
  template <class T> T function(const char *name) {
    auto symbol = reinterpret_cast<T>(dlsym(library_, name));
    if (!symbol)
      fail(ENOTSUP);
    return symbol;
  }
};
struct MacApi {
  Framework cf{"/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"};
  Framework services{"/System/Library/Frameworks/CoreServices.framework/CoreServices"};
  decltype(&CFStringCreateWithFileSystemRepresentation) string =
      cf.function<decltype(string)>("CFStringCreateWithFileSystemRepresentation");
  const CFArrayCallBacks *callbacks =
      cf.function<const CFArrayCallBacks *>("kCFTypeArrayCallBacks");
  decltype(&CFArrayCreate) array = cf.function<decltype(array)>("CFArrayCreate");
  decltype(&CFRelease) release = cf.function<decltype(release)>("CFRelease");
  decltype(&FSEventStreamCreate) create =
      services.function<decltype(create)>("FSEventStreamCreate");
  decltype(&FSEventStreamSetDispatchQueue) schedule =
      services.function<decltype(schedule)>("FSEventStreamSetDispatchQueue");
  decltype(&FSEventStreamStart) start = services.function<decltype(start)>("FSEventStreamStart");
  decltype(&FSEventStreamStop) stop = services.function<decltype(stop)>("FSEventStreamStop");
  decltype(&FSEventStreamInvalidate) invalidate =
      services.function<decltype(invalidate)>("FSEventStreamInvalidate");
  decltype(&FSEventStreamRelease) dispose =
      services.function<decltype(dispose)>("FSEventStreamRelease");
};
class Backend {
  static MacApi &api() {
    // Worker cleanup can continue during process exit.
    static auto *value = new MacApi;
    return *value;
  }
  Events &events_;
  fs::path root_;
  bool recursive_;
  dispatch_queue_t queue_ = nullptr;
  FSEventStreamRef stream_ = nullptr;
  bool started_ = false;
  static void receive(ConstFSEventStreamRef, void *context, size_t count, void *paths,
                      const FSEventStreamEventFlags flags[],
                      const FSEventStreamEventId[]) noexcept {
    auto &self = *static_cast<Backend *>(context);
    try {
      for (size_t i = 0; i < count; ++i) {
        constexpr auto lost =
            kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagKernelDropped |
            kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagRootChanged |
            kFSEventStreamEventFlagEventIdsWrapped;
        if (flags[i] & lost) {
          self.events_.rescan();
          continue;
        }
        if (flags[i] & kFSEventStreamEventFlagHistoryDone)
          continue;
        const fs::path path(static_cast<char **>(paths)[i]);
        auto relative = path.lexically_relative(self.root_);
        if (relative.empty() || *relative.begin() == "..")
          continue;
        if (relative == ".")
          relative.clear();
        if (self.recursive_ || relative.parent_path().empty())
          self.events_.changed(relative);
      }
    } catch (...) {
      self.events_.rescan();
    }
  }
  void close() noexcept {
    if (stream_) {
      if (started_)
        api().stop(stream_);
      api().invalidate(stream_);
      // No queued callback may retain this object after close returns.
      dispatch_sync_f(queue_, nullptr, [](void *) {});
      api().dispose(stream_);
    }
    if (queue_)
      dispatch_release(queue_);
  }

public:
  Backend(const fs::path &root, bool recursive, Events &events, const std::atomic<bool> &)
      : events_(events), root_(root), recursive_(recursive) {
    auto &a = api();
    auto name = a.string(nullptr, root.c_str());
    if (!name)
      fail(EILSEQ);
    const void *path = name;
    auto paths = a.array(nullptr, &path, 1, a.callbacks);
    if (!paths) {
      a.release(name);
      throw std::bad_alloc();
    }
    FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
    stream_ = a.create(nullptr, receive, &context, paths, kFSEventStreamEventIdSinceNow, 0.02,
                       kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                           kFSEventStreamCreateFlagWatchRoot);
    a.release(paths);
    a.release(name);
    if (!stream_)
      fail(EIO);
    queue_ = dispatch_queue_create("kalandra.fs.watch", DISPATCH_QUEUE_SERIAL);
    if (!queue_) {
      a.dispose(stream_);
      throw std::bad_alloc();
    }
    a.schedule(stream_, queue_);
    started_ = a.start(stream_);
    if (!started_) {
      close();
      fail(EIO);
    }
  }
  ~Backend() { close(); }
  void poll(const std::atomic<bool> &) {}
};
