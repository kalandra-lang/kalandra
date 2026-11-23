#include "../runtime/FileSystem.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace kalandra::runtime;
using namespace std::chrono_literals;

namespace {
void check(bool passed, const char *what) {
  if (!passed) {
    std::fprintf(stderr, "%s\n", what);
    std::abort();
  }
}
ProcessResult call(Invocation &ctx, FileAction action, const ProcessArguments &args) {
  ProcessResult out{};
  kalandra_file_v2(&ctx, &out, unsigned(action), &args);
  check(!ctx.poll() && out.kind == FileKind::Success, "TCP operation failed");
  return out;
}
void cancelRead(Invocation &owner, FileDescriptor endpoint) {
  Invocation caller;
  std::atomic<bool> stop{false};
  caller.bindCancellation(&stop);
  check(caller.retainFrom(owner, endpoint.storage.allocated), "borrow socket in child");
  BufferResult buffer{};
  BufferArguments create{};
  create.value = 16;
  kalandra_buffer_v1(&caller, &buffer, unsigned(kalandra::BufferAction::Create), &create);
  ProcessArguments args{};
  args.file = endpoint;
  args.buffer = buffer.buffer;
  args.value = 16;
  void *state = nullptr;
  ProcessResult out{};
  check(!kalandra_file_step_v2(&caller, &out, unsigned(FileAction::StreamRead), &args, &state) &&
            state,
        "empty socket should suspend");
  stop = true;
  check(kalandra_file_step_v2(&caller, &out, unsigned(FileAction::StreamRead), &args, &state) &&
            !state,
        "cancelled read retained operation");
  check(caller.poll() == -1 && !out.count, "wrong cancellation outcome");
}
void cycle() {
  Invocation ctx;
  auto *scope = kalandra_file_scope_enter_v1(&ctx, nullptr, 0);
  ProcessArguments address{};
  kalandra_string_literal_v1(&ctx, &address.path, "127.0.0.1", 9);
  ProcessArguments listener{};
  listener.file = call(ctx, FileAction::TcpListen, address).file;
  address.value = call(ctx, FileAction::TcpLocalPort, listener).count;
  address.mode = UINT64_MAX;
  ProcessArguments client{};
  client.file = call(ctx, FileAction::TcpConnect, address).file;
  auto server = call(ctx, FileAction::TcpAccept, listener).file;
  cancelRead(ctx, server);
  ProcessArguments halves{};
  halves.file = call(ctx, FileAction::TcpSplit, client).file;
  auto reader = call(ctx, FileAction::TcpTakeReader, halves).file;
  (void)call(ctx, FileAction::TcpTakeWriter, halves);
  cancelRead(ctx, reader);
  kalandra_file_scope_leave_v1(&ctx, scope, nullptr);
}
} // namespace

int main() {
  const auto baseline = fileServiceStats().reserved;
  for (unsigned count = 0; count < 32; ++count)
    cycle();
  auto until = std::chrono::steady_clock::now() + 5s;
  while (fileServiceStats().reserved != baseline && std::chrono::steady_clock::now() < until)
    std::this_thread::sleep_for(1ms);
  auto stats = fileServiceStats();
  check(stats.reserved == baseline && !stats.pendingClose, "socket cleanup leaked a handle slot");
  std::puts("32 scope exits and 64 cancelled socket reads released every handle slot");
}
