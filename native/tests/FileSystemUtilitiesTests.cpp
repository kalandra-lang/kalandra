#include "../runtime/FileSystem.h"
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
} // namespace
void testFileSystemUtilities(const std::filesystem::path &root) {
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
    auto removed = call(ctx, FileAction::RemoveDirectory, pathArgument(ctx, directory));
    require(removed.kind == FileKind::System, "nonempty directory was removed");
    removed = call(ctx, FileAction::RemoveFile, pathArgument(ctx, directory / "child"));
    require(removed.kind == FileKind::System && std::filesystem::is_directory(directory / "child"),
            "removeFile removed a directory");
    const auto created = directory / "created";
    require(call(ctx, FileAction::CreateDirectory, pathArgument(ctx, created)).kind ==
                FileKind::Success,
            "createDir failed");
    require(call(ctx, FileAction::CreateDirectory, pathArgument(ctx, created)).kind ==
                FileKind::System,
            "createDir silently accepted an existing path");
    require(call(ctx, FileAction::RemoveDirectory, pathArgument(ctx, created)).kind ==
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
