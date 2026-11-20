// Included in the filesystem service's private namespace. All traversal and
// native handle destruction occur on its I/O workers.
namespace fs = std::filesystem;
using tree::InvalidTreePath;
using tree::treeLocation;
using tree::treeSystemError;
struct InvalidTreeEncoding {};
struct TreeResourceLimit {};
struct TreeSlot {
  std::size_t slot = reserveFileSlot();
  TreeSlot() {
    if (slot == noFileSlot)
      throw TreeResourceLimit{};
  }
  ~TreeSlot() { releaseFileSlot(slot); }
  TreeSlot(const TreeSlot &) = delete;
  TreeSlot &operator=(const TreeSlot &) = delete;
};

#ifdef _WIN32
#include "WindowsDirectoryTree.inc"
#else
struct TreeDirectory {
  TreeSlot slot;
  DIR *dir = nullptr;
  ~TreeDirectory() {
    if (dir)
      ::closedir(dir);
  }
  int fd() const { return dirfd(dir); }
  bool next(fs::path &name, std::uint64_t &kind) {
    for (;;) {
      errno = 0;
      auto *entry = readdir(dir);
      if (!entry) {
        if (errno)
          treeSystemError(errno);
        return false;
      }
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      name = entry->d_name;
      struct stat info;
      if (fstatat(fd(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW))
        treeSystemError(errno);
      kind = S_ISREG(info.st_mode) ? 1 : S_ISDIR(info.st_mode) ? 2 : S_ISLNK(info.st_mode) ? 3 : 4;
      return true;
    }
  }
};
std::unique_ptr<TreeDirectory> treeOpen(const TreeDirectory *parent, const fs::path &name) {
  auto frame = std::make_unique<TreeDirectory>();
  const int fd = openat(parent ? parent->fd() : AT_FDCWD, name.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    treeSystemError(errno);
  if (!(frame->dir = fdopendir(fd))) {
    const int error = errno;
    ::close(fd);
    treeSystemError(error);
  }
  return frame;
}
void checkDeleteDirectory(const TreeDirectory &parent, const TreeDirectory &dir) {
  struct stat up, down, systemRoot;
  if (fstat(parent.fd(), &up) || fstat(dir.fd(), &down) || stat("/", &systemRoot))
    treeSystemError(errno);
  if (up.st_dev != down.st_dev || up.st_ino == down.st_ino ||
      (down.st_dev == systemRoot.st_dev && down.st_ino == systemRoot.st_ino))
    throw InvalidTreePath{};
}
std::unique_ptr<TreeDirectory> treeDeleteEntry(TreeDirectory &parent, const fs::path &name,
                                               bool root, bool /*directoryHint*/) {
  struct stat info;
  if (fstatat(parent.fd(), name.c_str(), &info, AT_SYMLINK_NOFOLLOW))
    treeSystemError(errno);
  if (S_ISDIR(info.st_mode)) {
    auto dir = treeOpen(&parent, name);
    checkDeleteDirectory(parent, *dir);
    return dir;
  }
  if (root && !S_ISLNK(info.st_mode))
    throw InvalidTreePath{};
  if (unlinkat(parent.fd(), name.c_str(), 0))
    treeSystemError(errno);
  return {};
}
void treeFinishDelete(TreeDirectory &parent, const fs::path &name, TreeDirectory &dir) {
  struct stat openInfo, current;
  if (fstat(dir.fd(), &openInfo) ||
      fstatat(parent.fd(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW))
    treeSystemError(errno);
  if (openInfo.st_dev != current.st_dev || openInfo.st_ino != current.st_ino)
    treeSystemError(EBUSY);
  if (unlinkat(parent.fd(), name.c_str(), AT_REMOVEDIR))
    treeSystemError(errno);
}
#endif

struct TreeFrame {
  std::unique_ptr<TreeDirectory> dir;
  fs::path name;
  std::size_t prefixLength;
};
struct TreeCursor {
  std::vector<TreeFrame> frames;
  fs::path prefix, pending;
  std::unique_ptr<TreeDirectory> rootParent;

  TreeCursor(const fs::path &path, bool removing, const std::atomic<bool> &stop) : prefix(path) {
    auto loc = treeLocation(path, removing);
    auto dir = treeOpen(nullptr, loc.anchor);
    for (std::size_t i = 0; i < loc.components.size(); ++i) {
      if (stop)
        return;
      const auto &part = loc.components[i];
      if (removing && i + 1 == loc.components.size()) {
        rootParent = std::move(dir);
        dir = treeDeleteEntry(*rootParent, part, true, true);
      } else
        dir = treeOpen(dir.get(), part);
    }
    if (dir)
      frames.push_back(
          {std::move(dir), loc.components.empty() ? fs::path{} : loc.components.back(), 0});
  }
  // A directory is yielded before opening its children on the next request.
  bool advance(std::string *out, std::uint64_t &kind, const std::atomic<bool> &stop) {
    while (!frames.empty() && !stop) {
      auto &top = frames.back();
      if (!pending.empty()) {
        auto child = treeOpen(top.dir.get(), pending);
        auto length = prefix.native().size();
        prefix /= pending;
        frames.push_back({std::move(child), std::exchange(pending, {}), length});
        continue;
      }
      fs::path name;
      if (top.dir->next(name, kind)) {
        if (stop)
          return false;
        if (out) {
          auto text = (prefix / name).u8string();
          out->assign(reinterpret_cast<const char *>(text.data()), text.size());
          if (kalandra::validateUtf8(*out, [&] { return !stop.load(); }) !=
              kalandra::Utf8Status::Valid)
            throw InvalidTreeEncoding{};
          if (kind == 2)
            pending = std::move(name);
          return true;
        }
        if (auto child = treeDeleteEntry(*top.dir, name, false, kind == 2))
          frames.push_back({std::move(child), std::move(name), 0});
      } else {
        if (stop)
          return false;
        if (!out) {
          auto &parent = frames.size() == 1 ? *rootParent : *frames[frames.size() - 2].dir;
          treeFinishDelete(parent, top.name, *top.dir);
        } else if (frames.size() > 1) {
          prefix = prefix.native().substr(0, top.prefixLength);
        }
        frames.pop_back();
      }
    }
    return false;
  }
};

void createParents(const fs::path &path, const std::atomic<bool> &stop) {
  const auto loc = treeLocation(path, false);
  auto current = loc.anchor;
  if (loc.components.empty() && !fs::is_directory(current))
    throw InvalidTreePath{};
  for (const auto &component : loc.components) {
    if (stop)
      return;
    current /= component;
    fs::create_directory(current);
  }
}
