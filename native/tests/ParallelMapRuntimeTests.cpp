#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_parallel_root;
extern void (*const kalandra_parallel_original)(Task &);
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Array {
  void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0, count = 0, stride = 0;
};
struct Result {
  Header header;
  Array value;
};
static_assert(sizeof(Result) == 72);
static std::atomic<unsigned> active, peak, calls;
static std::atomic<bool> overlap, hold, timedOut;
static unsigned workers;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
extern "C" void kalandra_parallel_probe(Task &task) {
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
  kalandra_parallel_original(task);
  --active;
}
static void balanced(Invocation &value) {
  value.settle();
  require(!value.live() && !value.environmentBytes() && !value.callDepth() &&
              value.allocations() == value.releases(),
          "parallel map leaked invocation ownership");
}
int main() {
  try {
    const auto &entry = *kalandra_parallel_root;
    require(entry.packetBytes == 0 && entry.result->bytes == sizeof(Result),
            "parallel-map probe ABI changed");
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
        limits.records = 16;
        limits.groups = 3;
        limits.ready = mode == 3 ? 1 : 8;
        limits.inFlight = 9;
        auto core = TaskCore::create(limits);
        require(bool(core), "parallel-map core unavailable");
        auto group = core->openRoot();
        Invocation source, observer;
        auto task = core->submit(group, entry, {entry.packetLayout, nullptr, 0, &source});
        require(bool(task) && core->close(group), "parallel-map root admission failed");
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
        require(core->drain(), "parallel-map drain failed");
        if (stopper.joinable())
          stopper.join();
        require(!timedOut && !active, "parallel-map overlap/cancellation probe timed out");
        Result result;
        if (!mode) {
          require(core->failure(task).kind == TaskFailure::None &&
                      core->observe(task, *entry.result, observer, &result),
                  "parallel-map result unavailable under bounded quotas");
          if (result.header.kind || result.value.count != 257 || result.value.stride != 1 ||
              result.value.offset)
            std::cerr << "descriptor: " << result.header.kind << ' ' << result.value.offset << ' '
                      << result.value.count << ' ' << result.value.stride << '\n';
          require(!result.header.kind && result.value.count == 257 && result.value.stride == 1 &&
                      !result.value.offset,
                  "parallel-map result descriptor changed");
          const auto *values = static_cast<const std::int64_t *>(result.value.aligned);
          for (std::int64_t i = 0; i < 257; ++i)
            require(values[i] == 2 * i, "parallel map reordered results");
          require(calls == 257 && core->stats().publications == 258,
                  "parallel map repeated/skipped callbacks");
          require(workers == 1 ? peak == 1 : peak >= 2, "parallel map callbacks did not overlap");
        } else if (mode == 3) {
          require(core->observe(task, *entry.result, observer, &result) &&
                      result.header.kind ==
                          static_cast<std::int32_t>(TaskFailure::AdmissionRejected) &&
                      result.value.count == 0,
                  "parallel map lost quota rejection or exposed partial output");
        } else {
          require(stopped &&
                      core->failure(group).kind ==
                          (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
                  "parallel map lost cancellation/deadline failure");
          result.value.count = 999;
          require(!core->observe(task, *entry.result, observer, &result) &&
                      result.value.count == 999,
                  "cancelled map published a partial array");
        }
        require(core->release(group), "parallel-map root release failed");
        const auto stats = core->stats();
        require(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedBytes &&
                    !stats.environmentBytes,
                "parallel map leaked scheduler reservations");
        if (!mode)
          require(static_cast<const std::int64_t *>(result.value.aligned)[256] == 512,
                  "parallel-map result did not survive producer release");
        balanced(observer);
        balanced(source);
      }
    }
    std::cout << "Parallel map: ordered 257-element results, overlapping callbacks, bounded "
                 "records, cancellation/deadlines and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
