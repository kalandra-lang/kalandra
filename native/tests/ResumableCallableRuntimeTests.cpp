#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_resumable_root;
extern void (*const kalandra_resumable_original)(Task &);
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Result {
  Header header;
  std::int64_t value = 0;
};
struct Callable {
  Header header;
  void *code = nullptr, *environment = nullptr;
};
static_assert(sizeof(Result) == 40 && sizeof(Callable) == 48);
extern "C" {
extern const ValueLayout *const kalandra_resumable_layout;
extern void *const kalandra_resumable_code;
extern void *const kalandra_resumable_wrong;
}
static std::atomic<unsigned> active, peak, calls;
static std::atomic<bool> overlap, hold, timedOut;
static unsigned workers;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
extern "C" void kalandra_resumable_probe(Task &task) {
  const auto current = active.fetch_add(1) + 1;
  auto previous = peak.load();
  while (previous < current && !peak.compare_exchange_weak(previous, current)) {
  }
  ++calls;
  if (current >= 2)
    overlap = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((hold.load() || (workers > 1 && !overlap.load())) && !task.cancelled()) {
    if (std::chrono::steady_clock::now() > deadline) {
      timedOut = true;
      break;
    }
    std::this_thread::yield();
  }
  if (task.releaseGroup({}))
    timedOut = true;
  kalandra_resumable_original(task);
  --active;
}
static void balanced(Invocation &value) {
  value.settle();
  require(!value.live() && !value.environmentBytes() && !value.callDepth() &&
              value.allocations() == value.releases(),
          "resumable callable leaked invocation ownership");
}
int main() {
  try {
    const auto &entry = *kalandra_resumable_root;
    require(entry.packetBytes == 0 && entry.result->bytes == sizeof(Result),
            "resumable-callable probe ABI changed");
    for (workers = 1; workers <= 4; workers *= 2) {
      for (unsigned mode = 0; mode < 4; ++mode) {
        active = 0;
        peak = 0;
        calls = 0;
        overlap = false;
        hold = mode == 1 || mode == 2;
        timedOut = false;
        TaskLimits limits;
        limits.workers = workers;
        limits.records = mode == 3 ? 1 : 16;
        limits.groups = 8;
        limits.ready = 8;
        limits.inFlight = 9;
        auto core = TaskCore::create(limits);
        require(bool(core), "resumable-callable core unavailable");
        auto group = core->openRoot();
        Invocation source, observer;
        auto task = core->submit(group, entry, {entry.packetLayout, nullptr, 0, &source});
        require(bool(task) && core->close(group), "resumable-callable root admission failed");
        std::jthread stopper;
        std::atomic<bool> stopped = false;
        if (mode == 1 || mode == 2)
          stopper = std::jthread([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!active.load() && std::chrono::steady_clock::now() < deadline)
              std::this_thread::yield();
            stopped = core->cancel(group, mode == 2);
            hold = false;
          });
        require(core->drain(), "resumable-callable drain failed");
        if (stopper.joinable())
          stopper.join();
        require(!timedOut && !active, "resumable-callable overlap/cancellation probe timed out");
        Result result;
        if (!mode) {
          require(core->failure(task).kind == TaskFailure::None &&
                      core->observe(task, *entry.result, observer, &result),
                  "resumable-callable result unavailable under bounded quotas");
          require(!result.header.kind && result.value == 42,
                  "resumable callable returned an invalid result");
          require(calls == 2 && core->stats().publications == 5,
                  "resumable callable repeated/skipped child calls");
          require(workers == 1 ? peak == 1 : peak >= 2,
                  "resumable callable callbacks did not overlap");
        } else if (mode == 3) {
          require(core->observe(task, *entry.result, observer, &result) &&
                      result.header.kind ==
                          static_cast<std::int32_t>(TaskFailure::AdmissionRejected) &&
                      result.value == 0,
                  "resumable callable lost quota rejection or exposed partial output");
        } else {
          require(stopped &&
                      core->failure(group).kind ==
                          (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
                  "resumable callable lost cancellation/deadline failure");
          result.value = 999;
          require(!core->observe(task, *entry.result, observer, &result) && result.value == 999,
                  "cancelled map published a partial array");
        }
        require(core->release(group), "resumable-callable root release failed");
        const auto stats = core->stats();
        require(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedBytes &&
                    !stats.environmentBytes,
                "resumable callable leaked scheduler reservations");
        if (!mode)
          require(result.value == 42, "resumable-callable result did not survive producer release");
        balanced(observer);
        balanced(source);
      }
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
      Invocation source, destination;
      Callable value;
      value.code = kalandra_resumable_code;
      value.environment = source.allocateEnvironment(sizeof(std::int64_t), value.code);
      require(value.environment, "probe environment allocation failed");
      *static_cast<std::int64_t *>(value.environment) = 1;
      if (mode == 1)
        value.code = nullptr;
      if (mode == 2)
        value.code = kalandra_resumable_wrong;
      if (mode == 3)
        value.environment = reinterpret_cast<void *>(1);
      if (mode == 4)
        value.header.kind = 1;
      if (mode == 5) {
        value.environment =
            source.allocateEnvironment(sizeof(std::int64_t), kalandra_resumable_wrong);
        require(value.environment, "wrong-key probe allocation failed");
      }
      Callable copied;
      const bool ok = kalandra_resumable_layout->copy(destination, &copied, source, &value);
      require(ok == (mode == 0), "resumable callable metadata validation failed");
      if (ok) {
        require(copied.code == value.code && copied.environment != value.environment &&
                    *static_cast<std::int64_t *>(copied.environment) == 1,
                "resumable capture environment was not independently copied");
        source.settle();
        require(*static_cast<std::int64_t *>(copied.environment) == 1,
                "resumable capture depended on producer storage");
      }
      balanced(source);
      balanced(destination);
    }
    std::cout << "Resumable callables: captured environments, overlapping child work, bounded "
                 "records, cancellation/deadlines and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
