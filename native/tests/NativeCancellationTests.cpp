#include "../runtime/Tasks.h"
#include "../runtime/NativeCancellation.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include "NativeCheckpointRegisters.h"

using namespace kalandra::runtime;
extern "C" bool kalandra_source_run_v4(Invocation *, bool (*)(void *), void *) noexcept;
extern "C" [[noreturn]] void kalandra_source_abort_v2();
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Native cancellation assertion %d\n", __LINE__); std::abort(); } } while (false)
#define NATIVE __attribute__((section("__TEXT,__kalandra,regular,pure_instructions"), noinline))
static std::atomic<unsigned> started{0}, foreignReleased{0}, completed{0};
static bool available = false;
struct Arguments {
  Invocation *invocation;
  bool cooperative, nested, fault, finite;
  std::uint64_t result = 0;
};
static NATIVE bool compute(void *opaque) {
  auto *args = static_cast<Arguments *>(opaque);
  started.fetch_add(1, std::memory_order_release);
  std::uint64_t value = 0;
  for (std::uint64_t i = 0; !args->finite || i != 10000; ++i) {
    value += i;
    if (args->cooperative && args->invocation->poll()) return false;
    asm volatile("" : "+r"(value));
  }
  args->result = value;
  return true;
}
extern "C" bool kalandra_test_borrow_register(bool (*)(void *), void *);
asm(R"(
.text
.globl _kalandra_test_borrow_register
.p2align 2
_kalandra_test_borrow_register:
  .cfi_startproc
  stp x29, x30, [sp, #-32]!
  .cfi_def_cfa_offset 32
  .cfi_offset w29, -32
  .cfi_offset w30, -24
  str x28, [sp, #16]
  .cfi_offset w28, -16
  mov x29, sp
  mov x16, x0
  mov x0, x1
  mov x28, #0xbad
  blr x16
  cmp x28, #0xbad
  b.ne 1f
  ldr x28, [sp, #16]
  ldp x29, x30, [sp], #32
  ret
1:
  brk #1
  .cfi_endproc
)");
static __attribute__((noinline)) bool foreignCallback(void *opaque) {
  auto *args = static_cast<Arguments *>(opaque);
  struct Owner {
    ~Owner() { foreignReleased.fetch_add(1, std::memory_order_relaxed); }
  } owner;
  if (args->fault) {
    args->invocation->fault(4, 7, 8);
    kalandra_source_abort_v2();
  }
  return kalandra_source_run_v4(args->invocation, compute, args);
}
static NATIVE bool nested(void *opaque) {
  return kalandra_test_borrow_register(foreignCallback, opaque);
}
static bool construct(Invocation &, void *frame, const Packet &) {
  *static_cast<unsigned *>(frame) = 0;
  return true;
}
static void destroy(void *) noexcept {}
static void resume(Task &task) {
  auto &invocation = task.context();
  // Physical-worker signal masks choose capability, not just process setup.
  const bool cooperative = !available || !nativeCancellationThread();
  Arguments args{&invocation, cooperative, true, false, false};
  if ((*static_cast<unsigned *>(task.frame()))++ == 0) {
    args.finite = true;
    CHECK(kalandra_source_run_v4(&invocation, compute, &args));
    CHECK(args.result == 49995000);
    task.yield();
    return;
  }
  CHECK(invocation.allocate(1024));
  if (kalandra_source_run_v4(&invocation, nested, &args)) std::abort();
}
static bool copy(Invocation &, void *output, const Invocation &, const void *input) noexcept {
  std::memcpy(output, input, sizeof(std::uint64_t));
  return true;
}
static ValueLayout layout{72, sizeof(std::uint64_t), alignof(std::uint64_t), copy, destroy};
static TaskEntry entry{1, 71, 0, 1, sizeof(unsigned), alignof(unsigned), construct, destroy, resume, &layout};
static void waitStarted(unsigned before) {
  const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (started.load(std::memory_order_acquire) == before) {
    CHECK(std::chrono::steady_clock::now() < timeout);
    std::this_thread::yield();
  }
}
static void occupied(int) {}
struct CheckpointProbe {
  std::atomic<bool> entered{false};
  Invocation *invocation;
};
static bool checkpointProbe(void *opaque) {
  auto *probe = static_cast<CheckpointProbe *>(opaque);
  return kalandra_source_run_v4(probe->invocation, kalandra_test_checkpoint_spin, &probe->entered);
}
int main(int argc, char **argv) {
  bool blocked = argc > 1 && std::strcmp(argv[1], "blocked") == 0;
  bool conflict = argc > 1 && std::strcmp(argv[1], "occupied") == 0;
  if (conflict) std::signal(SIGUSR2, occupied);
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR2);
  if (blocked) CHECK(pthread_sigmask(SIG_BLOCK, &mask, nullptr) == 0);
  available = initializeNativeCancellation();
  CHECK(available == (!conflict && nativeCancellationCanAbandon()));
  if (available && !blocked) {
    for (unsigned repeat = 0; repeat != 64; ++repeat) {
      Invocation invocation;
      std::atomic<bool> token{false}, done{false};
      invocation.bindCancellation(&token);
      invocation.allowNativeInterrupts(true);
      CheckpointProbe probe{{false}, &invocation};
      const auto current = nativeCancellationThread();
      std::thread cancel([&] {
        while (!probe.entered.load(std::memory_order_acquire)) std::this_thread::yield();
        token.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) {
          interruptNativeCancellation(current);
          std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
      });
      CHECK(!kalandra_test_checkpoint_registers(checkpointProbe, &probe));
      done.store(true, std::memory_order_release);
      cancel.join();
      CHECK(!invocation.status() && !invocation.callDepth());
    }
  }
  for (unsigned workers : {1U, 4U}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 2;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (unsigned repeat = 0; repeat != 100; ++repeat) {
      auto group = core->openRoot();
      auto task = core->submit(group, entry, {71, nullptr, 0});
      CHECK(task && core->close(group));
      CHECK(core->step());
      const auto before = started.load();
      std::thread canceller([&] {
        waitStarted(before);
        CHECK(core->cancel(group, repeat % 2));
      });
      CHECK(core->drain());
      canceller.join();
      CHECK(core->settled(task));
      CHECK(core->failure(group).kind == (repeat % 2 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
      CHECK(core->release(group));
      CHECK(core->stats().retainedOwners == 0 && core->stats().retainedBytes == 0);
    }
    if (workers > 1) CHECK(core->stats().migrations >= 100);
  }
  CHECK(foreignReleased.load() == 200);
  if (available && !blocked) CHECK(nativeCancellationRedirects() >= 200);
  // A fault must unwind the foreign owner rather than jump over it.
  {
    Invocation invocation;
    std::atomic<bool> token{false};
    invocation.bindCancellation(&token);
    invocation.allowNativeInterrupts(available && !blocked);
    Arguments args{&invocation, !available || blocked, true, true, false};
    CHECK(!kalandra_source_run_v4(&invocation, nested, &args));
    CHECK(invocation.status() == 4 && invocation.line() == 7 && invocation.column() == 8);
    CHECK(invocation.callDepth() == 0 && foreignReleased.load() == 201);
  }
  // Spam delayed interrupts while one physical pthread changes invocation,
  // token binding and capability. A stale request cannot cancel fresh work.
  if (available && !blocked) {
    std::atomic<bool> stop{false};
    const auto current = nativeCancellationThread();
    std::thread late([&] {
      while (!stop.load(std::memory_order_acquire)) {
        interruptNativeCancellation(current);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    });
    for (unsigned repeat = 0; repeat != 1000; ++repeat) {
      Invocation invocation;
      std::atomic<bool> token{false};
      if (repeat % 3) invocation.bindCancellation(&token);
      invocation.allowNativeInterrupts(repeat % 2);
      Arguments args{&invocation, false, false, false, true};
      CHECK(kalandra_source_run_v4(&invocation, compute, &args));
      CHECK(args.result == 49995000 && invocation.callDepth() == 0 && !invocation.status());
      completed.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_release);
    late.join();
    CHECK(completed.load() == 1000);
  }
  std::printf("Native cancellation: nested foreign cleanup, worker reuse/migration, deadlines, faults and late interrupts pass; async=%d redirects=%llu\n",
              available && !blocked, static_cast<unsigned long long>(nativeCancellationRedirects()));
  if (conflict) {
    struct sigaction action{};
    CHECK(sigaction(SIGUSR2, nullptr, &action) == 0 && action.sa_handler == occupied);
  }
}
