#pragma once
#include "../runtime/DirectoryWatch.h"
#include <chrono>
#include <fstream>
#include <set>
#include <thread>

namespace directory_watch_tests {
namespace fs = std::filesystem;
namespace watch = kalandra::runtime::watch;
using namespace std::chrono_literals;

template <class Predicate>
void until(watch::DirectoryWatch &watcher, const std::atomic<bool> &stop, Predicate accept,
           const char *failure) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  do {
    if (auto event = watcher.next(stop); event && accept(*event))
      return;
    std::this_thread::sleep_for(5ms);
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(failure);
}

inline void exercise(const fs::path &root) {
  fs::create_directories(root / "child");
  std::atomic<bool> stop{false};
  watch::DirectoryWatch direct(root, false, stop);
  watch::DirectoryWatch recursive(root, true, stop);
  std::ofstream(root / "child" / "nested") << "nested";
  until(
      recursive, stop,
      [&](const auto &e) {
        return e.kind == watch::Kind::Changed && e.path == root / "child" / "nested";
      },
      "recursive watcher missed nested file");
  std::ofstream(root / "top") << "direct";
  until(
      direct, stop,
      [&](const auto &e) {
        if (e.path == root / "child" / "nested")
          throw std::runtime_error("direct scope saw descendant");
        return e.path == root / "top";
      },
      "direct watcher missed immediate child");

  // One pending detail is enough to exercise the same bounded delivery path
  // as the production limit without changing the host's OS configuration.
  watch::DirectoryWatch small(root, true, stop, 1);
  for (unsigned i = 0; i < 512; ++i)
    std::ofstream(root / std::to_string(i)) << i;
  until(
      small, stop, [&](const auto &e) { return e.kind == watch::Kind::Rescan && e.path == root; },
      "overflow failed to produce Rescan(root)");
  std::set<fs::path> actual, expected = {"child", "top"};
  for (unsigned i = 0; i < 512; ++i)
    expected.emplace(std::to_string(i));
  for (const auto &entry : fs::directory_iterator(root))
    actual.insert(entry.path().filename());
  if (actual != expected)
    throw std::runtime_error("rescan did not reconstruct directory contents");
  // Drain earlier OS batches before producing a distinct, post-recovery change.
  auto quiet = std::chrono::steady_clock::now();
  auto deadline = quiet + 10s;
  while (std::chrono::steady_clock::now() - quiet < 150ms) {
    if (std::chrono::steady_clock::now() > deadline)
      throw std::runtime_error("watch never settled");
    if (small.next(stop))
      quiet = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(5ms);
  }
  std::ofstream(root / "recovered") << "new";
  until(
      small, stop,
      [&](const auto &e) { return e.kind == watch::Kind::Changed && e.path == root / "recovered"; },
      "same watcher missed event after rescan");

  // A directory populated before a recursive backend can subscribe must be
  // discoverable through a change hint or a rescan. Later writes remain visible.
  watch::DirectoryWatch expanding(root, true, stop);
  fs::create_directory(root / "later");
  until(
      expanding, stop,
      [&](const auto &e) { return e.kind == watch::Kind::Rescan || e.path == root / "later"; },
      "recursive watcher missed new directory");
  std::ofstream(root / "later" / "visible") << "new";
  until(
      expanding, stop, [&](const auto &e) { return e.path == root / "later" / "visible"; },
      "new directory did not receive a subscription");
}
} // namespace directory_watch_tests
