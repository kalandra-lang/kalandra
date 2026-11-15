#include "../runtime/FileSystem.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace kalandra::runtime;
using namespace std::string_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "file assertion %d: %s\n", __LINE__, #x);                               \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static StringDescriptor text(Invocation &ctx, const std::string &s) {
  StringDescriptor value;
  kalandra_string_literal_v1(&ctx, &value, s.data(), s.size());
  CHECK(!ctx.status());
  return value;
}
static FileBytes bytes(Invocation &ctx, const std::string &s) {
  FileBytes value;
  value.sizes[0] = s.size();
  value.strides[0] = 1;
  if (!s.empty()) {
    value.allocated = value.aligned = static_cast<std::uint8_t *>(ctx.allocate(s.size()));
    CHECK(value.allocated);
    std::memcpy(value.aligned, s.data(), s.size());
  }
  return value;
}
static std::string contents(const FileBytes &s) {
  return s.sizes[0] ? std::string(reinterpret_cast<const char *>(s.aligned), s.sizes[0]) : "";
}
static FileResult call(Invocation &ctx, FileAction action, FileArguments args = {}) {
  FileResult result;
  kalandra_file_v1(&ctx, &result, std::uint32_t(action), &args);
  CHECK(!ctx.status() && !ctx.poll());
  return result;
}
static FileDescriptor open(Invocation &ctx, const std::filesystem::path &path, FileMode mode) {
  FileArguments args;
  const auto encoded = path.u8string();
  args.path =
      text(ctx, std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size()));
  args.mode = std::uint64_t(mode);
  auto result = call(ctx, FileAction::Open, args);
  CHECK(result.kind == FileKind::Success && result.file.storage.allocated);
  return result.file;
}
static FileArguments withFile(FileDescriptor file, std::uint64_t n = 0) {
  FileArguments args;
  args.file = file;
  args.value = n;
  return args;
}
static void consumingHandleTransfer(const std::filesystem::path &root) {
  for (unsigned mode = 0; mode != 4; ++mode) {
    Invocation destination;
    void *receivingScope = nullptr;
    if (mode & 1)
      receivingScope = kalandra_file_scope_enter_v1(&destination, nullptr, 0);
    FileDescriptor file;
    {
      Invocation source;
      if (mode & 2)
        CHECK(ensureOwnedEscapeScope(source));
      auto *scope = kalandra_file_scope_enter_v1(&source, nullptr, 0);
      CHECK(scope);
      file = open(source, root / ("consuming-" + std::to_string(mode)), FileMode::Create);
      {
        OwnedReceiveGuard receiving(source, destination);
        CHECK(kalandra_owned_copy_v1(&destination, &source, &file));
      }
      kalandra_file_scope_leave_v1(&source, scope, nullptr);
      source.settle();
      CHECK(!source.live());
    }
    auto args = withFile(file);
    args.bytes = bytes(destination, "still open");
    auto result = call(destination, FileAction::WriteAll, args);
    CHECK(result.kind == FileKind::Success && result.count == 10);
    if (receivingScope) {
      kalandra_file_scope_leave_v1(&destination, receivingScope, nullptr);
      CHECK(call(destination, FileAction::WriteAll, args).kind == FileKind::InvalidArgument);
    }
    destination.settle();
    CHECK(!destination.live() && destination.allocations() == destination.releases());
  }
}
static void roundTrip(const std::filesystem::path &root) {
  Invocation ctx;
  const auto path = root / std::filesystem::path(std::u8string(u8"spaces Ω 文.bin"));
  auto file = open(ctx, path, FileMode::CreateNew);
  auto args = withFile(file);
  args.bytes = bytes(ctx, std::string("α\0β\r\nlast", 12));
  auto result = call(ctx, FileAction::WriteAll, args);
  CHECK(result.kind == FileKind::Success && result.count == 12);
  CHECK(call(ctx, FileAction::Close, withFile(file)).kind == FileKind::Success);
  CHECK(call(ctx, FileAction::Close, withFile(file)).kind == FileKind::InvalidArgument);
  file = open(ctx, path, FileMode::Read);
  result = call(ctx, FileAction::ReadExact, withFile(file, 12));
  CHECK(result.kind == FileKind::Success && contents(result.bytes) == contents(args.bytes));
  result = call(ctx, FileAction::Read, withFile(file, 100));
  CHECK(result.kind == FileKind::Success && result.count == 0 && result.bytes.sizes[0] == 0);
  CHECK(call(ctx, FileAction::SeekTo, withFile(file, 0)).kind == FileKind::Success);
  result = call(ctx, FileAction::ReadExact, withFile(file, 15));
  CHECK(result.kind == FileKind::UnexpectedEnd && result.count == 12);
  CHECK(call(ctx, FileAction::SeekTo, withFile(file, 2)).kind == FileKind::Success);
  auto destination = bytes(ctx, "????????????????");
  args = withFile(file);
  args.bytes = destination;
  result = call(ctx, FileAction::ReadInto, args);
  CHECK(result.kind == FileKind::Success && result.count == 10);
  CHECK(contents(destination).substr(10) == "??????");
  CHECK(call(ctx, FileAction::Close, withFile(file)).kind == FileKind::Success);
  ctx.settle();
  CHECK(!ctx.live() && ctx.allocations() == ctx.releases());
}
static void bufferingAndLines(const std::filesystem::path &root) {
  Invocation ctx;
  auto file = open(ctx, root / "lines", FileMode::Create);
  auto result = call(ctx, FileAction::BufferedWriter, withFile(file, 7));
  CHECK(result.kind == FileKind::Success);
  auto writer = result.file;
  CHECK(call(ctx, FileAction::WriteAll, withFile(file)).kind == FileKind::InvalidArgument);
  auto args = withFile(writer);
  const std::string data = "α\0β\r\nlonggg\n\xff\nlast\r"s;
  args.bytes = bytes(ctx, data);
  CHECK(call(ctx, FileAction::WriteAll, args).count == data.size());
  CHECK(call(ctx, FileAction::Finish, withFile(writer)).kind == FileKind::Success);
  file = open(ctx, root / "lines", FileMode::Read);
  result = call(ctx, FileAction::BufferedReader, withFile(file, 3));
  CHECK(result.kind == FileKind::Success);
  auto reader = result.file;
  result = call(ctx, FileAction::ReadLine, withFile(reader, 5));
  CHECK(result.kind == FileKind::Success && result.count == 5);
  CHECK(std::string(result.text.data, result.text.byteLength) == "α\0β"s);
  CHECK(call(ctx, FileAction::ReadLine, withFile(reader, 2)).kind == FileKind::LineTooLong);
  CHECK(call(ctx, FileAction::ReadLine, withFile(reader, 5)).kind == FileKind::InvalidEncoding);
  result = call(ctx, FileAction::ReadLine, withFile(reader, 5));
  CHECK(result.kind == FileKind::Success &&
        std::string(result.text.data, result.text.byteLength) == "last\r");
  CHECK(call(ctx, FileAction::ReadLine, withFile(reader, 0)).kind == FileKind::End);
  CHECK(call(ctx, FileAction::SeekTo, withFile(reader, 0)).kind == FileKind::Success);
  CHECK(call(ctx, FileAction::ReadExact, withFile(reader, 2)).kind == FileKind::Success);
  CHECK(call(ctx, FileAction::SeekTo, withFile(reader, UINT64_MAX)).kind ==
        FileKind::InvalidArgument);
  CHECK(call(ctx, FileAction::ReadLine, withFile(reader, 3)).kind == FileKind::Success);
  ctx.settle();
  CHECK(!ctx.live());
  // Cancellation/scope cleanup must discard output that was never flushed.
  {
    Invocation cancelled;
    file = open(cancelled, root / "discarded", FileMode::Create);
    writer = call(cancelled, FileAction::BufferedWriter, withFile(file, 100)).file;
    args = withFile(writer);
    args.bytes = bytes(cancelled, "discard me");
    CHECK(call(cancelled, FileAction::WriteAll, args).count == 10);
    cancelled.fault(1);
    cancelled.settle();
  }
  CHECK(std::filesystem::file_size(root / "discarded") == 0);
}
static void logicalOwnership(const std::filesystem::path &root) {
  Invocation ctx;
  void *outer = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(outer);
  auto file = open(ctx, root / "logical-owner", FileMode::Create);
  auto writer = call(ctx, FileAction::BufferedWriter, withFile(file, 128)).file;
  auto args = withFile(writer);
  args.bytes = bytes(ctx, "must not flush");
  CHECK(call(ctx, FileAction::WriteAll, args).kind == FileKind::Success);
  const FileDescriptor *owned[] = {&writer};
  void *inner = kalandra_file_scope_enter_v1(&ctx, owned, 1);
  CHECK(inner);
  kalandra_file_scope_leave_v1(&ctx, inner, nullptr);
  // The caller still has a physical descriptor/backing. Logical ownership,
  // rather than the last retained reference, nevertheless closed its OS file.
  CHECK(call(ctx, FileAction::Flush, withFile(writer)).kind == FileKind::InvalidArgument);
  CHECK(std::filesystem::file_size(root / "logical-owner") == 0);
  file = open(ctx, root / "logical-owner", FileMode::Read);
  owned[0] = &file;
  inner = kalandra_file_scope_enter_v1(&ctx, owned, 1);
  CHECK(inner);
  kalandra_file_scope_leave_v1(&ctx, inner, &file);
  CHECK(call(ctx, FileAction::Read, withFile(file, 1)).kind == FileKind::Success);
  // A borrowing scope must leave the caller's ownership intact.
  inner = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  CHECK(inner);
  kalandra_file_scope_leave_v1(&ctx, inner, nullptr);
  CHECK(call(ctx, FileAction::Read, withFile(file, 1)).kind == FileKind::Success);
  kalandra_file_scope_leave_v1(&ctx, outer, nullptr);
  CHECK(call(ctx, FileAction::Read, withFile(file, 1)).kind == FileKind::InvalidArgument);
  ctx.settle();
  CHECK(!ctx.live());
}
static void workerOwnershipHandoff(const std::filesystem::path &root) {
  Invocation parent, child;
  auto *outer = kalandra_file_scope_enter_v1(&parent, nullptr, 0);
  CHECK(outer);
  auto file = open(parent, root / "worker-owner", FileMode::Create);
  CHECK(child.retainFrom(parent, file.storage.allocated));
  const FileDescriptor *owned[] = {&file};
  auto *inner = kalandra_file_scope_enter_v1(&child, owned, 1);
  CHECK(inner);
  kalandra_file_scope_leave_v1(&child, inner, &file);
  child.settle();
  CHECK(kalandra_file_receive_v1(&parent, &file));
  kalandra_file_scope_leave_v1(&parent, outer, nullptr);
  CHECK(call(parent, FileAction::WriteAll, withFile(file)).kind == FileKind::InvalidArgument);
  parent.settle();
  CHECK(!parent.live() && !child.live());
}
static void largeTextWrites(const std::filesystem::path &root) {
  const std::string payload(131073, 'x');
  for (const auto capacity : {0u, 7u, 65536u}) {
    Invocation ctx;
    const auto path = root / ("large-text-" + std::to_string(capacity));
    auto file = open(ctx, path, FileMode::Create);
    if (capacity)
      file = call(ctx, FileAction::BufferedWriter, withFile(file, capacity)).file;
    auto args = withFile(file);
    args.path = text(ctx, payload);
    auto result = call(ctx, FileAction::WriteText, args);
    CHECK(result.kind == FileKind::Success && result.count == payload.size());
    result = call(ctx, FileAction::WriteLine, args);
    CHECK(result.kind == FileKind::Success && result.count == payload.size() + 1);
    CHECK(call(ctx, capacity ? FileAction::Finish : FileAction::Close, withFile(file)).kind ==
          FileKind::Success);
    file = open(ctx, path, FileMode::Read);
    result = call(ctx, FileAction::ReadExact, withFile(file, payload.size() * 2 + 1));
    CHECK(result.kind == FileKind::Success && contents(result.bytes) == payload + payload + "\n");
    ctx.settle();
    CHECK(!ctx.live());
  }
}
static void serviceRetirement() {
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    const auto state = fileServiceStats();
    CHECK(state.reserved <= 65536 && state.pendingClose <= state.reserved && state.workers <= 64);
    if (!state.reserved)
      break;
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
static void deepResourceScopesBody(const std::filesystem::path &root) {
  Invocation ctx;
  auto file = open(ctx, root / "deep-owned-scopes", FileMode::Create);
  const FileDescriptor *owned[] = {&file};
  std::vector<void *> scopes;
  scopes.reserve(50000);
  for (unsigned n = 0; n < 50000; ++n) {
    auto *scope = kalandra_file_scope_enter_v1(&ctx, owned, 1);
    CHECK(scope);
    scopes.push_back(scope);
  }
  for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
    kalandra_file_scope_leave_v1(&ctx, *it, nullptr);
  // A retained file must not keep all retired scope ancestors alive, turning
  // final backing release into native recursion proportional to source depth.
  ctx.settle();
  CHECK(!ctx.live());
}
static void deepResourceScopes(const std::filesystem::path &root) {
#ifdef _WIN32
  std::thread([&] { deepResourceScopesBody(root); }).join();
#else
  pthread_attr_t attributes;
  CHECK(pthread_attr_init(&attributes) == 0);
  CHECK(pthread_attr_setstacksize(&attributes, 512 * 1024) == 0);
  auto path = root;
  pthread_t thread;
  CHECK(pthread_create(
            &thread, &attributes,
            [](void *value) -> void * {
              deepResourceScopesBody(*static_cast<std::filesystem::path *>(value));
              return nullptr;
            },
            &path) == 0);
  CHECK(pthread_attr_destroy(&attributes) == 0);
  CHECK(pthread_join(thread, nullptr) == 0);
#endif
}
static void errorsAndFailures(const std::filesystem::path &root) {
  {
    Invocation ctx;
    FileArguments args;
    args.path = text(ctx, (root / "missing").string());
    CHECK(call(ctx, FileAction::Open, args).kind == FileKind::System);
    args.path = text(ctx, "bad\0path"s);
    CHECK(call(ctx, FileAction::Open, args).kind == FileKind::InvalidArgument);
    args.path = text(ctx, (root / "exists").string());
    args.mode = std::uint64_t(FileMode::CreateNew);
    CHECK(call(ctx, FileAction::Open, args).kind == FileKind::Success);
    CHECK(call(ctx, FileAction::Open, args).kind == FileKind::System);
    ctx.settle();
    CHECK(!ctx.live());
  }
  for (unsigned failure = 1; failure <= 8; ++failure) {
    Invocation ctx;
    FileArguments args;
    args.path = text(ctx, (root / "exists").string());
    ctx.failAllocationAt(failure);
    FileResult result;
    kalandra_file_v1(&ctx, &result, std::uint32_t(FileAction::Open), &args);
    CHECK(!ctx.status() || ctx.status() == 2);
    ctx.settle();
    CHECK(!ctx.live() && ctx.allocations() == ctx.releases());
  }
  // An ordinary owned numeric allocation cannot impersonate a resource.
  {
    Invocation ctx;
    auto raw = bytes(ctx, std::string(1024, '\0'));
    FileArguments args;
    args.file.storage = raw;
    FileResult result;
    kalandra_file_v1(&ctx, &result, std::uint32_t(FileAction::Close), &args);
    CHECK(ctx.status() == 3);
    ctx.settle();
    CHECK(!ctx.live());
  }
}
#ifndef _WIN32
static void blockedReadCancellation(const std::filesystem::path &root) {
  const auto path = root / "blocked-read.fifo";
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  const auto peer = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
  CHECK(peer >= 0);
  Invocation ctx;
  CHECK(kalandra_file_scope_enter_v1(&ctx, nullptr, 0));
  auto file = open(ctx, path, FileMode::Read);
  std::atomic<bool> cancelled{false};
  ctx.bindCancellation(&cancelled);
  auto args = withFile(file, 128);
  void *state = nullptr;
  FileResult result;
  CHECK(!kalandra_file_step_v1(&ctx, &result, std::uint32_t(FileAction::Read), &args, &state));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  cancelled = true;
  const auto start = std::chrono::steady_clock::now();
  CHECK(kalandra_file_step_v1(&ctx, &result, std::uint32_t(FileAction::Read), &args, &state));
  ctx.settle();
  CHECK(!ctx.live() && !state);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  // Source ownership metadata is gone while the OS request still has no EOF.
  // Completing that request must neither access freed metadata nor leak its fd.
  CHECK(::close(peer) == 0);
}
static void dependentFileOperations(const std::filesystem::path &root) {
  // More blocked reads than a tiny fixed pool can service. Their sibling opens
  // must still run; otherwise every worker waits for queued work and deadlocks.
  constexpr std::size_t count = 8;
  std::array<Invocation, count> readers;
  std::array<FileArguments, count> arguments;
  std::array<FileResult, count> results;
  std::array<void *, count> states{};
  std::array<std::filesystem::path, count> paths;
  for (std::size_t i = 0; i < count; ++i) {
    paths[i] = root / ("dependent-" + std::to_string(i) + ".fifo");
    CHECK(::mkfifo(paths[i].c_str(), 0600) == 0);
    arguments[i].path = text(readers[i], paths[i].string());
    CHECK(!kalandra_file_step_v1(&readers[i], &results[i], std::uint32_t(FileAction::Open),
                                 &arguments[i], &states[i]));
  }
  Invocation writer;
  for (const auto &path : paths) {
    auto file = open(writer, path, FileMode::Write);
    CHECK(call(writer, FileAction::Close, withFile(file)).kind == FileKind::Success);
  }
  writer.settle();
  for (std::size_t i = 0; i < count; ++i) {
    while (!kalandra_file_step_v1(&readers[i], &results[i], std::uint32_t(FileAction::Open),
                                  &arguments[i], &states[i]))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(results[i].kind == FileKind::Success);
    readers[i].settle();
    CHECK(!readers[i].live());
  }
}
static void blockedCancellation(const std::filesystem::path &root) {
  const auto path = root / "blocked.fifo";
  CHECK(::mkfifo(path.c_str(), 0600) == 0);
  Invocation ctx;
  std::atomic<bool> cancelled{false};
  ctx.bindCancellation(&cancelled);
  FileArguments args;
  args.path = text(ctx, path.string());
  void *state = nullptr;
  FileResult result;
  CHECK(!kalandra_file_step_v1(&ctx, &result, std::uint32_t(FileAction::Open), &args, &state));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  cancelled = true;
  const auto start = std::chrono::steady_clock::now();
  CHECK(kalandra_file_step_v1(&ctx, &result, std::uint32_t(FileAction::Open), &args, &state));
  CHECK(!state);
  ctx.settle();
  CHECK(!ctx.live());
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  // Release the service's outstanding OS call only after source state retired.
  int peer = -1;
  for (unsigned n = 0; n < 100 && peer < 0; ++n) {
    peer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (peer < 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(peer >= 0);
  CHECK(::close(peer) == 0);
}
#endif
int main(int argc, char **argv) {
  CHECK(argc == 2);
  const std::filesystem::path root(argv[1]);
  std::filesystem::create_directories(root);
  std::filesystem::remove_all(root / "cases");
  std::filesystem::create_directory(root / "cases");
  roundTrip(root / "cases");
  consumingHandleTransfer(root / "cases");
  bufferingAndLines(root / "cases");
  errorsAndFailures(root / "cases");
  logicalOwnership(root / "cases");
  workerOwnershipHandoff(root / "cases");
  largeTextWrites(root / "cases");
  deepResourceScopes(root / "cases");
#ifndef _WIN32
  dependentFileOperations(root / "cases");
  blockedReadCancellation(root / "cases");
  blockedCancellation(root / "cases");
#endif
  serviceRetirement();
  std::filesystem::remove_all(root / "cases");
  std::puts("Owned filesystem runtime: bytes, lines, buffering, errors and cancellation passed");
}
