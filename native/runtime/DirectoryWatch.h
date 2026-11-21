#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <unordered_map>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kalandra::runtime::watch {
namespace fs = std::filesystem;
enum class Kind { Changed = 1, Rescan };
struct Event {
  Kind kind;
  fs::path path;
};

// A rescan supersedes all pending details. Once delivered, later changes can
// accumulate while the consumer rebuilds its view.
class Events {
  const fs::path root_;
  const std::size_t limit_;
  std::mutex mutex_;
  std::deque<fs::path> paths_;
  bool lost_ = false;

public:
  Events(fs::path root, std::size_t limit) : root_(std::move(root)), limit_(limit) {}
  void rescan() {
    std::lock_guard lock(mutex_);
    lost_ = true;
    paths_.clear();
  }
  void changed(const fs::path &relative) {
    std::lock_guard lock(mutex_);
    if (lost_)
      return;
    if (!paths_.empty() && relative == paths_.back())
      return;
    if (paths_.size() >= limit_) {
      lost_ = true;
      paths_.clear();
    } else
      paths_.push_back(relative);
  }
  std::optional<Event> pop() {
    std::lock_guard lock(mutex_);
    if (std::exchange(lost_, false))
      return Event{Kind::Rescan, root_};
    if (paths_.empty())
      return {};
    Event result{Kind::Changed, paths_.front().empty() ? root_ : root_ / paths_.front()};
    paths_.pop_front();
    return result;
  }
};
[[noreturn]] inline void fail(int code) { throw std::system_error(code, std::system_category()); }
#if defined(__linux__)
#include "DirectoryWatchLinux.inc"
#elif defined(_WIN32)
#include "DirectoryWatchWindows.inc"
#elif defined(__APPLE__)
#include "DirectoryWatchMac.inc"
#else
class Backend {
public:
  Backend(const fs::path &, bool, Events &, const std::atomic<bool> &) {
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported));
  }
  void poll(const std::atomic<bool> &) {}
};
#endif
class DirectoryWatch {
  Events events_;
  Backend backend_;
  static fs::path directory(const fs::path &path) {
    if (!fs::is_directory(fs::symlink_status(path)))
      throw std::system_error(std::make_error_code(std::errc::not_a_directory));
    return fs::canonical(path);
  }

public:
  DirectoryWatch(const fs::path &root, bool recursive, const std::atomic<bool> &stop,
                 std::size_t capacity = 256)
      : events_(root, capacity), backend_(directory(root), recursive, events_, stop) {}
  std::optional<Event> next(const std::atomic<bool> &stop) {
    if (auto ready = events_.pop())
      return ready;
    backend_.poll(stop);
    return events_.pop();
  }
};
} // namespace kalandra::runtime::watch
