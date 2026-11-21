#include "../runtime/FileSystem.h"
#include "../runtime/TreeLocation.h"
#include "DirectoryWatchTests.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
using namespace kalandra::runtime;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}
FileArguments pathArgument(Invocation &ctx, const std::filesystem::path &path) {
  const auto value = utf8(path);
  FileArguments argument;
  kalandra_string_literal_v1(&ctx, &argument.path, value.data(), value.size());
  return argument;
}
FileResult call(Invocation &ctx, FileAction action, FileArguments argument) {
  FileResult out;
  kalandra_file_v1(&ctx, &out, unsigned(action), &argument);
  return out;
}
void settle(Invocation &ctx) {
  ctx.settle();
  require(!ctx.live() && ctx.allocations() == ctx.releases(),
          "filesystem utility allocation leaked");
}
void drain() {
  for (unsigned i = 0; i != 5000; ++i) {
    const auto stats = fileServiceStats();
    if (!stats.reserved && !stats.pendingClose)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error("directory handle/deferred close slot leaked");
}
ProcessResult callTree(Invocation &ctx, FileAction action, const FileArguments &argument) {
  const ProcessArguments args{argument, nullptr};
  ProcessResult result{};
  kalandra_file_v2(&ctx, &result, unsigned(action), &args);
  return result;
}
void testIdleWatch(const std::filesystem::path &root) {
  for (bool faulted : {false, true}) {
    std::atomic<bool> stop{false};
    Invocation ctx;
    ctx.bindCancellation(&stop);
    const auto opened = callTree(ctx, FileAction::OpenWatch, pathArgument(ctx, root));
    require(opened.kind == FileKind::Success && !ctx.status(), "watch registration failed");
    ProcessArguments input{};
    input.file = opened.file;
    ProcessResult out{};
    void *state = nullptr;
    // Drive several service retries with no filesystem activity.
    auto quiet = std::chrono::steady_clock::now();
    const auto deadline = quiet + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() - quiet < std::chrono::milliseconds(60) || !state) {
      require(std::chrono::steady_clock::now() < deadline, "watch never became idle");
      if (kalandra_file_step_v2(&ctx, &out, unsigned(FileAction::ReadWatch), &input, &state)) {
        // Native backends may coalesce earlier changes to the root into a hint.
        require(out.kind == FileKind::Success && !ctx.status(), "watch failed before cancellation");
        quiet = std::chrono::steady_clock::now();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (faulted)
      ctx.fault(7);
    stop.store(true);
    require(kalandra_file_step_v2(&ctx, &out, unsigned(FileAction::ReadWatch), &input, &state) &&
                !state && !out.text.allocated && ctx.status() == (faulted ? 7 : 0),
            "idle cancellation published data or lost fault precedence");
    settle(ctx);
    drain();
  }
}
void testTreeCursor(const std::filesystem::path &parent) {
  namespace fs = std::filesystem;
  // Exercise root rejection through the same decomposition used by removeTree,
  // without ever passing a real filesystem root to a destructive operation.
  std::vector<fs::path> protectedPaths = {"", ".", "..", "local/.", "local/.."};
#ifdef _WIN32
  protectedPaths.insert(protectedPaths.end(), {LR"(C:\)", LR"(\\server\share)", LR"(\\?\C:\)",
                                               LR"(\\?\UNC\server\share\)"});
#else
  protectedPaths.insert(protectedPaths.end(), {"/", "///", "/."});
#endif
  for (const auto &path : protectedPaths) {
    bool rejected = false;
    try {
      (void)tree::treeLocation(path, true);
    } catch (const tree::InvalidTreePath &) {
      rejected = true;
    }
    require(rejected, "removal accepted a filesystem root or ambiguous leaf");
  }
  const auto root = fs::canonical(parent) / "lazy-tree";
  const auto branch = root / "branch";
  fs::create_directories(branch / "nested");
  {
    Invocation ctx;
    auto opened = callTree(ctx, FileAction::OpenWalk, pathArgument(ctx, root));
    require(!ctx.status() && opened.kind == FileKind::Success, "tree open failed");
    ProcessArguments arg{};
    arg.file = opened.file;
    auto first = callTree(ctx, FileAction::ReadDirectory, arg);
    require(first.kind == FileKind::Success && first.code == 2 &&
                std::string(first.text.data, first.text.byteLength) == utf8(branch),
            "walker did not publish its first directory");
    // Descent is lazy. Change the yielded directory before the next request.
    fs::rename(branch, root / "moved");
#ifndef _WIN32
    fs::create_directory_symlink(root / "moved", branch);
#endif
    auto next = callTree(ctx, FileAction::ReadDirectory, arg);
    require(next.kind == FileKind::System, "walker followed a replacement or lost its error");
    settle(ctx);
    drain();
  }
  std::atomic<bool> stop{false};
  {
    Invocation ctx;
    ctx.bindCancellation(&stop);
    auto opened = callTree(ctx, FileAction::OpenWalk, pathArgument(ctx, root / "moved"));
    ProcessArguments arg{};
    arg.file = opened.file;
    require(callTree(ctx, FileAction::ReadDirectory, arg).kind == FileKind::Success,
            "failed to enter partial walk");
    void *state = nullptr;
    ProcessResult out{};
    (void)kalandra_file_step_v2(&ctx, &out, unsigned(FileAction::ReadDirectory), &arg, &state);
    stop = true;
    require(kalandra_file_step_v2(&ctx, &out, unsigned(FileAction::ReadDirectory), &arg, &state) &&
                !state && !out.text.allocated,
            "cancelled walk published an entry");
    settle(ctx);
    drain();
  }
  {
    Invocation ctx;
    auto path = root;
    for (unsigned i = 0; i < 48; ++i)
      path /= "d";
    require(callTree(ctx, FileAction::DirectoryCreateAll, pathArgument(ctx, path)).kind ==
                FileKind::Success,
            "deep parent creation failed");
    require(callTree(ctx, FileAction::RemoveTree, pathArgument(ctx, root)).kind ==
                FileKind::Success,
            "deep tree removal failed");
    require(!fs::exists(root), "recursive removal left its root");
    settle(ctx);
    drain();
  }
}
} // namespace
void testFileSystemUtilities(const std::filesystem::path &root) {
  const auto watchRoot = std::filesystem::canonical(root) / "watch";
  directory_watch_tests::exercise(watchRoot);
  const auto idle = watchRoot / "idle";
  std::filesystem::create_directory(idle);
  testIdleWatch(idle);
  std::filesystem::remove_all(watchRoot);
  testTreeCursor(root);
  const auto directory = root / std::filesystem::path(std::u8string(u8"directory Ω"));
  std::filesystem::create_directory(directory);
  std::filesystem::create_directory(directory / "child");
  {
    std::ofstream output(directory / "sample.bin", std::ios::binary);
    output << "hello";
  }
  {
    std::ofstream output(directory / std::filesystem::path(std::u8string(u8"é 文.txt")));
  }
  {
    Invocation ctx;
    auto info = call(ctx, FileAction::Metadata, pathArgument(ctx, directory / "sample.bin"));
    require(!ctx.status() && info.kind == FileKind::Success && info.code == 1 && info.count == 5,
            "regular file metadata changed");
    info = call(ctx, FileAction::Metadata, pathArgument(ctx, directory));
    require(info.kind == FileKind::Success && info.code == 2 && info.count == 0,
            "directory metadata changed");
    auto opened = call(ctx, FileAction::OpenDirectory, pathArgument(ctx, directory));
    require(opened.kind == FileKind::Success, "directory open failed");
    FileArguments input;
    input.file = opened.file;
    std::map<std::string, int> entries;
    StringDescriptor retained;
    for (;;) {
      auto next = call(ctx, FileAction::ReadDirectory, input);
      if (next.kind == FileKind::End)
        break;
      require(!ctx.status() && next.kind == FileKind::Success, "directory enumeration failed");
      const std::string name(next.text.data, next.text.byteLength);
      require(!entries.contains(name), "directory entry repeated");
      entries[name] = next.code;
      retained = next.text;
    }
    require(entries ==
                std::map<std::string, int>{
                    {"child", 2},
                    {"sample.bin", 1},
                    {utf8(std::filesystem::path(std::u8string(u8"é 文.txt"))), 1}},
            "directory names, kind, encoding or dot-entry exclusion changed");
    require(call(ctx, FileAction::ReadDirectory, input).kind == FileKind::End,
            "directory exhaustion is not stable");
    require(call(ctx, FileAction::Close, input).kind == FileKind::Success,
            "directory close failed");
    require(validateString(ctx, retained) && retained.byteLength > 0,
            "close invalidated yielded name");
    require(call(ctx, FileAction::ReadDirectory, input).kind == FileKind::InvalidArgument,
            "closed directory accepted advancement");
    auto removed = call(ctx, FileAction::DirectoryRemove, pathArgument(ctx, directory));
    require(removed.kind == FileKind::System, "nonempty directory was removed");
    removed = call(ctx, FileAction::RemoveFile, pathArgument(ctx, directory / "child"));
    require(removed.kind == FileKind::System && std::filesystem::is_directory(directory / "child"),
            "removeFile removed a directory");
    const auto created = directory / "created";
    require(call(ctx, FileAction::DirectoryCreate, pathArgument(ctx, created)).kind ==
                FileKind::Success,
            "createDir failed");
    require(call(ctx, FileAction::DirectoryCreate, pathArgument(ctx, created)).kind ==
                FileKind::System,
            "createDir silently accepted an existing path");
    require(call(ctx, FileAction::DirectoryRemove, pathArgument(ctx, created)).kind ==
                FileKind::Success,
            "removeDir failed");
    require(call(ctx, FileAction::Metadata, pathArgument(ctx, created)).kind == FileKind::System,
            "missing metadata succeeded");
    settle(ctx);
  }
  drain();
#ifndef _WIN32
  {
    const auto link = directory / "link";
    std::filesystem::create_directory_symlink(directory / "child", link);
    Invocation ctx;
    for (const auto &path : {link, std::filesystem::path(link.string() + "/")}) {
      auto info = call(ctx, FileAction::Metadata, pathArgument(ctx, path));
      require(info.kind == FileKind::Success && info.code == 3 && info.count == 0,
              "metadata followed a final symlink");
    }
    require(call(ctx, FileAction::RemoveFile, pathArgument(ctx, link)).kind == FileKind::Success &&
                std::filesystem::is_directory(directory / "child"),
            "symlink removal affected target");
    settle(ctx);
  }
  {
    const auto invalid = directory / std::string("invalid-\xff", 9);
    errno = 0;
    std::ofstream invalidOutput(invalid);
    if (!invalidOutput.is_open()) {
      require(errno == EILSEQ, "invalid-name fixture failed for an unexpected OS reason");
      std::puts("NOTE: filesystem rejects invalid UTF-8 names; rejection-on-enumeration requires "
                "Linux qualification");
    } else {
      invalidOutput.close();
      Invocation ctx;
      FileArguments input;
      input.file = call(ctx, FileAction::OpenDirectory, pathArgument(ctx, directory)).file;
      bool rejected = false;
      for (unsigned i = 0; i != 10; ++i) {
        auto result = call(ctx, FileAction::ReadDirectory, input);
        if (result.kind == FileKind::InvalidEncoding) {
          rejected = true;
          break;
        }
        if (result.kind == FileKind::End)
          break;
        require(result.kind == FileKind::Success, "unexpected invalid-name error");
      }
      require(rejected, "invalid UTF-8 filename was silently replaced");
      settle(ctx);
      std::filesystem::remove(invalid);
    }
  }
  drain();
#endif
  // Construction and advancement fail at every allocator boundary, while
  // automatic resource retirement still releases native handles and slots.
  unsigned successes = 0, failures = 0;
  for (unsigned ordinal = 0; ordinal != 32; ++ordinal) {
    Invocation ctx;
    auto argument = pathArgument(ctx, directory);
    if (ordinal)
      ctx.failAllocationAt(ctx.allocations() + ordinal);
    auto opened = call(ctx, FileAction::OpenDirectory, argument);
    if (!ctx.status() && opened.kind == FileKind::Success) {
      FileArguments input;
      input.file = opened.file;
      (void)call(ctx, FileAction::ReadDirectory, input);
    }
    if (ctx.status()) {
      require(ctx.status() == 2, "allocation failure changed fault kind");
      ++failures;
    } else
      ++successes;
    settle(ctx);
    drain();
  }
  require(successes && failures, "allocation sweep missed a boundary");
  for (bool faulted : {false, true}) {
    Invocation ctx;
    auto argument = pathArgument(ctx, directory);
    std::atomic<bool> stop{false};
    ctx.bindCancellation(&stop);
    FileResult out;
    void *state = nullptr;
    (void)kalandra_file_step_v1(&ctx, &out, unsigned(FileAction::OpenDirectory), &argument, &state);
    if (faulted)
      ctx.fault(7);
    stop.store(true, std::memory_order_release);
    require(
        kalandra_file_step_v1(&ctx, &out, unsigned(FileAction::OpenDirectory), &argument, &state) &&
            !state && !out.file.storage.allocated && ctx.status() == (faulted ? 7 : 0),
        "directory cancellation changed fault precedence or published a handle");
    settle(ctx);
    drain();
  }
  std::filesystem::remove_all(directory);
}
