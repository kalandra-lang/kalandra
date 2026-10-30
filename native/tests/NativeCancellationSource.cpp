#include "../runtime/Tasks.h"
#include "../runtime/NativeCancellation.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace kalandra::runtime;
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Source cancellation assertion %d\n", __LINE__); std::abort(); } } while (false)
static std::atomic<unsigned> started{0};
static double expected(int kind, std::int64_t n) {
  std::int64_t value = 17;
  for (std::int64_t i = 0; i != n; ++i)
    value = kind == 0 ? (value * 13 + 7) % 1000003 :
            kind == 1 ? (i % 2 ? value * 13 + 7 : value * 7 + 11) % 1000003 :
                        (value * 14 + 1) % 1000003;
  return double(value + (kind == 2 ? 2 : 0));
}
static bool construct(Invocation &, void *frame, const Packet &packet) {
  std::memcpy(frame, packet.data, sizeof(int));
  return true;
}
static void destroy(void *) noexcept {}
static bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  std::memcpy(out, in, sizeof(double));
  return true;
}
static ValueLayout layout{99, sizeof(double), alignof(double), copy, destroy};
static void resume(Task &task) {
  auto &invocation = task.context();
  CHECK(invocation.allocate(4096));
  started.fetch_add(1, std::memory_order_release);
  const auto value = _mlir_ciface_kalandra_test_entry(*static_cast<int *>(task.frame()),
                                                   1000000000000000LL, &invocation);
  if (!task.cancelled()) task.complete(&value);
}
static TaskEntry entry{1, 98, sizeof(int), alignof(int), sizeof(int), alignof(int),
                       construct, destroy, resume, &layout};
int main() {
  const bool available = initializeNativeCancellation();
  unsigned numeric = 0, cancellations = 0;
  for (int mode : {0, 1, 3})
    for (int kind = 0; kind != 3; ++kind)
      for (int n : {0, 1, 14, 100, 1000, 10000}) {
        Invocation invocation;
        std::atomic<bool> token{false};
        if (mode) invocation.bindCancellation(&token);
        invocation.allowNativeInterrupts(available && mode == 3);
        CHECK(_mlir_ciface_kalandra_test_entry(kind, n, &invocation) == expected(kind, n));
        CHECK(!invocation.status() && invocation.callDepth() == 0);
        invocation.settle();
        CHECK(!invocation.live() && invocation.allocations() == invocation.releases());
        ++numeric;
      }
  for (unsigned workers : {1U, 4U}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 2;
    limits.bytes = 128 * 1024 * 1024;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (int kind = 0; kind != 3; ++kind)
      for (unsigned repeat = 0; repeat != 20; ++repeat) {
        const auto before = started.load();
        auto group = core->openRoot();
        auto task = core->submit(group, entry, {98, &kind, sizeof(kind)});
        CHECK(task && core->close(group));
        std::thread canceller([&] {
          const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (started.load(std::memory_order_acquire) == before) {
            CHECK(std::chrono::steady_clock::now() < end);
            std::this_thread::yield();
          }
          std::this_thread::sleep_for(std::chrono::microseconds(kind == 2 ? 100 : 1000));
          CHECK(core->cancel(group, repeat % 2));
        });
        CHECK(core->drain());
        canceller.join();
        CHECK(core->settled(task));
        if (core->failure(task).kind != TaskFailure::Cancelled)
          std::fprintf(stderr, "workers=%u kind=%d repeat=%u failure=%d code=%lld line=%lld column=%lld redirects=%llu\n",
                       workers, kind, repeat, int(core->failure(task).kind),
                       static_cast<long long>(core->failure(task).code),
                       static_cast<long long>(core->failure(task).line),
                       static_cast<long long>(core->failure(task).column),
                       static_cast<unsigned long long>(nativeCancellationRedirects()));
        CHECK(core->failure(task).kind == TaskFailure::Cancelled);
        CHECK(core->failure(group).kind == (repeat % 2 ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
        CHECK(core->release(group));
        CHECK(!core->stats().retainedBytes && !core->stats().retainedOwners);
        ++cancellations;
      }
  }
  if (available) CHECK(nativeCancellationRedirects() > 0);
  std::printf("Generated source: %u exact numerical cases, %u scheduler cancellations; async=%d redirects=%llu\n",
              numeric, cancellations, available,
              static_cast<unsigned long long>(nativeCancellationRedirects()));
}
