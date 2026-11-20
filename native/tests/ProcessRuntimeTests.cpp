#include "../runtime/FileSystem.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
using namespace kalandra::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", what);
    std::abort();
  }
}
StringDescriptor text(Invocation &ctx, const std::string &s) {
  StringDescriptor result;
  kalandra_string_literal_v1(&ctx, &result, s.data(), s.size());
  return result;
}
ProcessResult call(Invocation &ctx, FileAction action, const ProcessArguments &args) {
  ProcessResult result;
  kalandra_file_v2(&ctx, &result, unsigned(action), &args);
  check(!ctx.poll() && result.kind == FileKind::Success, "runtime operation failed");
  return result;
}
FileDescriptor start(Invocation &ctx, const std::string &program, const std::string &mode) {
  auto *arg = static_cast<StringDescriptor *>(ctx.allocateRecord(sizeof(StringDescriptor)));
  check(arg != nullptr, "allocate child arguments");
  *arg = text(ctx, mode);
  CommandView cmd{text(ctx, program), {arg, arg, 0, 1, 1}, {}, {}, {}, {2, 2, 3}};
  ProcessArguments args;
  args.command = &cmd;
  return call(ctx, FileAction::Start, args).file;
}
void checkCommandBudget(const std::string &program) {
  std::uint64_t used = 0;
  {
    Invocation ctx;
    ctx.bindRecursiveBudget(
        &used,
        [](void *p, std::uint64_t bytes) noexcept {
          auto &total = *static_cast<std::uint64_t *>(p);
          if (bytes > 65536 - total)
            return false;
          total += bytes;
          return true;
        },
        [](void *p, std::uint64_t bytes) noexcept { *static_cast<std::uint64_t *>(p) -= bytes; });
    constexpr std::int64_t count = 128;
    auto *cells =
        static_cast<StringDescriptor *>(ctx.allocateRecord(count * sizeof(StringDescriptor)));
    check(cells != nullptr, "allocate shared argument descriptors");
    std::fill_n(cells, count, text(ctx, std::string(8192, 'a')));
    CommandView cmd{text(ctx, program), {cells, cells, 0, count, 1}, {}, {}, {}, {3, 3, 3}};
    check(!ctx.poll(), "shared source arguments must fit the small budget");
    ProcessArguments args;
    args.command = &cmd;
    ProcessResult result;
    kalandra_file_v2(&ctx, &result, unsigned(FileAction::Start), &args);
    check(ctx.poll() == 2 && !result.file.storage.allocated, "expanded argv exceeds task budget");
  }
  check(used == 0, "rejected command retained budget");
}
void cancelPending(Invocation &owner, FileDescriptor handle, FileAction action) {
  Invocation caller;
  std::atomic<bool> stop{false};
  caller.bindCancellation(&stop);
  check(caller.retainFrom(owner, handle.storage.allocated), "retain borrowed endpoint");
  ProcessArguments args;
  args.file = handle;
  args.value = 1;
  BufferArguments create{};
  create.value = 1;
  BufferResult allocated{};
  if (action == FileAction::StreamRead || action == FileAction::StreamWriteAll) {
    kalandra_buffer_v1(&caller, &allocated, unsigned(kalandra::BufferAction::Create), &create);
    args.buffer = allocated.buffer;
    if (action == FileAction::StreamWriteAll) {
      BufferArguments append;
      append.buffer = args.buffer;
      constexpr int size = 512 * 1024;
      append.bytes.aligned = append.bytes.allocated =
          static_cast<unsigned char *>(caller.allocate(size));
      check(append.bytes.aligned != nullptr, "allocate backpressure input");
      std::memset(append.bytes.aligned, 42, size);
      append.bytes.sizes[0] = size;
      append.bytes.strides[0] = 1;
      kalandra_buffer_v1(&caller, &allocated, unsigned(kalandra::BufferAction::Append), &append);
    }
  }
  void *state = nullptr;
  ProcessResult result;
  for (int i = 0; i < 4; ++i) {
    check(!kalandra_file_step_v2(&caller, &result, unsigned(action), &args, &state),
          "call should remain pending");
    std::this_thread::sleep_for(2ms);
  }
  stop = true;
  check(kalandra_file_step_v2(&caller, &result, unsigned(action), &args, &state) && !state,
        "cancelled call retained request");
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "wait-for-eof") {
    while (std::getchar() != EOF) {
    }
    return 31;
  }
  if (argc == 2 && std::string(argv[1]) == "sleep") {
    std::this_thread::sleep_for(60s);
    return 1;
  }
  const auto program = std::filesystem::absolute(argv[0]).string();
  checkCommandBudget(program);
  {
    Invocation owner;
    void *scope = kalandra_file_scope_enter_v1(&owner, nullptr, 0);
    ProcessArguments child;
    child.file = start(owner, program, "wait-for-eof");
    auto writer = call(owner, FileAction::TakeStdin, child).file;
    auto reader = call(owner, FileAction::TakeStdout, child).file;
    cancelPending(owner, child.file, FileAction::Wait);
    cancelPending(owner, reader, FileAction::StreamRead);
    ProcessArguments endpoint;
    endpoint.file = writer;
    call(owner, FileAction::PipeClose, endpoint);
    check(call(owner, FileAction::Wait, child).count == 31, "cancelled waiter disturbed owner");
    check(call(owner, FileAction::Wait, child).count == 31, "native status lost");
    kalandra_file_scope_leave_v1(&owner, scope, nullptr);
  }
  {
    Invocation owner;
    auto *scope = kalandra_file_scope_enter_v1(&owner, nullptr, 0);
    ProcessArguments child;
    child.file = start(owner, program, "sleep");
    const auto writer = call(owner, FileAction::TakeStdin, child).file;
    cancelPending(owner, writer, FileAction::StreamWriteAll);
    auto reader = call(owner, FileAction::TakeStdout, child).file;
    kalandra_file_scope_leave_v1(&owner, scope, &reader);
    // The child never closes stdout voluntarily. EOF on the taken endpoint
    // proves that dropping its former owner terminated the direct child.
    ProcessArguments read;
    read.file = reader;
    read.value = 1;
    BufferArguments input{};
    input.value = 1;
    BufferResult allocated{};
    kalandra_buffer_v1(&owner, &allocated, unsigned(kalandra::BufferAction::Create), &input);
    read.buffer = allocated.buffer;
    check(call(owner, FileAction::StreamRead, read).count == 0,
          "taken endpoint lost EOF on cleanup");
  }
  for (int n = 0; n < 8; ++n) {
    Invocation owner;
    CommandView cmd{text(owner, program),
                    {},
                    text(owner, "kalandra-missing-child-directory"),
                    {},
                    {},
                    {2, 2, 2}};
    ProcessArguments args;
    args.command = &cmd;
    ProcessResult error;
    kalandra_file_v2(&owner, &error, unsigned(FileAction::Start), &args);
    check(!owner.poll() && error.kind == FileKind::System && error.code &&
              !error.file.storage.allocated,
          "failed start published an owner");
  }
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (fileServiceStats().reserved && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  check(fileServiceStats().reserved == 0, "child or pipe retained a close slot");
}
