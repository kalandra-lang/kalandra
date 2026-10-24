#include "../runtime/Runtime.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#if defined(__APPLE__)
#include <pthread.h>
#endif
using kalandra::runtime::Invocation;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
void check(bool value, int line) {
  if (!value)
    throw std::runtime_error("checked fast call regression at " + std::to_string(line));
}
#define CHECK(x) check(bool(x), __LINE__)
static auto call(Invocation &ctx, std::int64_t depth = 5) {
  return _mlir_ciface_kalandra_test_entry(17, depth, &ctx);
}
static std::int64_t expected(unsigned depth) {
  const auto count = std::int64_t(1) << depth;
  return count * (34 * count + count - 1) / 2;
}
#if defined(__APPLE__)
__attribute__((noinline)) static void lowStack(unsigned depth, Invocation &ctx) {
  // Dynamic alloca stays on the physical stack even with ASan's fake stack.
  const auto bytes = std::size_t(8192 + depth % 16);
  auto *memory = static_cast<volatile char *>(__builtin_alloca(bytes));
  for (std::size_t i = 0; i < bytes; i += 4096)
    memory[i] = char(depth);
  if (depth)
    lowStack(depth - 1, ctx);
  else
    CHECK(call(ctx, 0) == 0 && ctx.status() == 6 && ctx.callDepth() == 0);
  CHECK(memory[0] == char(depth));
}
struct Worker {
  Invocation *context;
  bool low;
  bool ok = false;
};
static void *worker(void *argument) {
  auto &test = *static_cast<Worker *>(argument);
  try {
    if (test.low)
      lowStack(18, *test.context);
    else
      CHECK(call(*test.context) == expected(5) && test.context->status() == 0 &&
            test.context->callDepth() == 1);
    test.ok = true;
  } catch (...) {
  }
  return nullptr;
}
#if defined(__aarch64__)
static void *boundedWorker(void *argument) {
  auto &test = *static_cast<Worker *>(argument);
  try {
    const auto high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    const auto low = high - pthread_get_stacksize_np(pthread_self()) + 128 * 1024;
    std::uintptr_t position;
    asm volatile("mov %0, sp" : "=r"(position));
    CHECK(position > low + 2048);
    const auto bytes = position - low - 1024;
    auto *memory = static_cast<volatile char *>(__builtin_alloca(bytes));
    for (std::size_t i = 0; i < bytes; i += 4096)
      memory[i] = char(i);
    // The outer entry has room, but deeper physical frames must still stop
    // before the reserve, even on the private context/cached-token paths.
    CHECK(call(*test.context, 1000) == 0 && test.context->status() == 6 &&
          test.context->callDepth() == 0);
    CHECK(memory[0] == 0);
    test.ok = true;
  } catch (...) {
  }
  return nullptr;
}
#endif
#endif
int main() {
  Invocation context;
  for (unsigned i = 0; i < 200; ++i) {
    CHECK(call(context) == expected(5));
    CHECK(context.status() == 0 && context.callDepth() == 0 && context.live() == 0);
  }
  std::atomic<std::uint64_t> probes = 0;
  context.watchPolls(&probes);
  CHECK(call(context) == expected(5) && probes > 63);
  context.watchPolls(nullptr);
  CHECK(call(context) == expected(5));
  {
    Invocation failed;
    failed.fault(5, 7, 9);
    CHECK(call(failed) == 0 && failed.status() == 5 && failed.line() == 7 && failed.column() == 9);
    CHECK(failed.callDepth() == 0);
  }
  {
    Invocation closed;
    closed.settle();
    CHECK(call(closed) == 0 && closed.status() == 3 && closed.callDepth() == 0);
  }
  {
    Invocation cancelled;
    std::atomic<bool> token = true;
    cancelled.bindCancellation(&token);
    CHECK(call(cancelled) == 0 && cancelled.status() == 0 && cancelled.poll() == -1);
    CHECK(cancelled.callDepth() == 0);
    token = false;
    CHECK(call(cancelled) == expected(5) && cancelled.callDepth() == 0);
  }
  {
    Invocation limited;
    for (unsigned i = 0; i < 4096; ++i)
      CHECK(limited.enterCall(1, 2));
    CHECK(call(limited) == expected(5) && limited.status() == 0);
    CHECK(limited.callDepth() == 4096);
    for (unsigned i = 0; i < 4096; ++i)
      limited.leaveCall();
    CHECK(limited.callDepth() == 0);
  }
  {
    Invocation cancelled;
    std::atomic<bool> token = false;
    cancelled.bindCancellation(&token);
    std::thread stop([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      token = true;
    });
    const auto value = call(cancelled, 22);
    stop.join();
    CHECK(value == 0 && cancelled.poll() == -1 && cancelled.callDepth() == 0);
  }
  {
    Invocation interrupted;
    std::atomic<bool> token = false;
    std::atomic<std::uint64_t> polls = 0;
    interrupted.bindCancellation(&token);
    interrupted.watchPolls(&polls);
    std::thread stop([&] {
      while (polls.load() < 1000)
        std::this_thread::yield();
      token = true;
    });
    CHECK(call(interrupted, 20) == 0);
    stop.join();
    CHECK(interrupted.poll() == -1 && interrupted.callDepth() == 0 && interrupted.live() == 0);
  }
#if defined(__APPLE__)
#if defined(__aarch64__)
  for (int mode : {0, 1, 2}) {
    Invocation bounded;
    std::atomic<bool> token = false;
    std::atomic<std::uint64_t> polls = 0;
    if (mode == 1)
      bounded.bindCancellation(&token);
    if (mode == 2)
      bounded.watchPolls(&polls);
    pthread_attr_t attributes;
    CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes, 256 * 1024));
    Worker test{&bounded, false};
    pthread_t thread;
    CHECK(!pthread_create(&thread, &attributes, boundedWorker, &test));
    CHECK(!pthread_join(thread, nullptr));
    CHECK(!pthread_attr_destroy(&attributes));
    CHECK(test.ok && bounded.status() == 6 && bounded.callDepth() == 0);
  }
#endif
  // Same Invocation, with an existing manual admission, migrates between stacks.
  // Generated chain leases must be released independently of logical depth zero.
  Invocation migrating;
  CHECK(migrating.enterCall(0, 0));
  for (unsigned i = 0; i < 12; ++i) {
    pthread_attr_t attributes;
    CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes, i % 2 ? 256 * 1024 : 512 * 1024));
    Worker test{&migrating, false};
    pthread_t thread;
    CHECK(!pthread_create(&thread, &attributes, worker, &test));
    CHECK(!pthread_join(thread, nullptr));
    CHECK(!pthread_attr_destroy(&attributes));
    CHECK(test.ok && migrating.callDepth() == 1);
    CHECK(call(migrating) == expected(5) && migrating.callDepth() == 1);
  }
  migrating.leaveCall();
  CHECK(migrating.callDepth() == 0);
  {
    Invocation low;
    pthread_attr_t attributes;
    CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setstacksize(&attributes, 256 * 1024));
    Worker test{&low, true};
    pthread_t thread;
    CHECK(!pthread_create(&thread, &attributes, worker, &test));
    CHECK(!pthread_join(thread, nullptr));
    CHECK(!pthread_attr_destroy(&attributes));
    CHECK(test.ok && low.status() == 6 && low.callDepth() == 0);
  }
#endif
  std::puts("Checked fast calls: status/probes/cancellation/depth/unwind/migration/bounded stacks "
            "passed");
}
