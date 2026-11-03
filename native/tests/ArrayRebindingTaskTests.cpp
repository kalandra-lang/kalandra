#include "../runtime/Tasks.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
#ifndef EXPECTED_BIAS
#define EXPECTED_BIAS 15
#endif
#ifndef EXPECTED_PER_PAIR
#define EXPECTED_PER_PAIR 5
#endif
extern "C" const TaskEntry *const kalandra_task_test_entry;
struct Value {
  Failure failure;
  std::int64_t value;
};
struct Input {
  Value rounds, faultAt;
};
static const TaskEntry *original;
static std::uint64_t allocationFailure;
static std::atomic<std::uint64_t> polls;
static bool construct(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(allocationFailure);
  context.watchPolls(&polls);
  return original->construct(context, frame, packet);
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    original = kalandra_task_test_entry;
    require(original && original->packetBytes == sizeof(Input) &&
                original->result->bytes == sizeof(Value),
            "array loop descriptor mismatch");
    auto entry = *original;
    entry.construct = construct;
    for (unsigned workers : {1u, 4u}) {
      for (unsigned mode = 0; mode < 68; ++mode) {
        TaskLimits limits;
        limits.workers = workers;
        limits.records = 16;
        limits.groups = 8;
        limits.ready = 8;
        limits.inFlight = 8;
        auto core = TaskCore::create(limits);
        require(bool(core), "array loop task core creation failed");
        const bool cancel = mode == 1;
        const bool fault = mode == 2;
        allocationFailure = mode >= 4 ? mode - 3 : 0;
        polls = 0;
        auto group = core->openRoot();
        Invocation source, destination;
        const std::int64_t rounds = cancel ? INT64_MAX : mode == 0 ? 2000 : 10;
        Input input{{{}, rounds}, {{}, fault ? 5 : -1}};
        auto task =
            core->submit(group, entry, {entry.packetLayout, &input, sizeof(input), &source});
        require(bool(task), "array loop submission failed");
        if (cancel) {
          bool drained = false;
          std::jthread drainer(
              [&] { drained = core->drain(std::numeric_limits<std::size_t>::max()); });
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (polls.load() < 1000 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
          const bool ran = polls >= 1000;
          const bool requested = core->cancel(group);
          drainer.join();
          require(ran && requested && drained, "array loop running cancellation failed");
          require(core->failure(group).kind == TaskFailure::Cancelled,
                  "array loop cancellation changed its error channel");
        } else {
          require(core->drain(), "array loop did not drain");
          auto failure = core->failure(task);
          if (failure.kind == TaskFailure::None) {
            Value result{};
            require(core->observe(task, *entry.result, destination, &result),
                    "array loop result observation failed");
            failure = result.failure;
            if (failure.kind == TaskFailure::None)
              require(!fault && result.value == EXPECTED_BIAS + rounds / 2 * EXPECTED_PER_PAIR,
                      "array replacement lost a saved alias or a task read snapshot");
          }
          if (fault)
            require(failure.kind == TaskFailure::Fault && failure.code == 4,
                    "array loop arithmetic fault was replaced during cleanup");
          else if (failure.kind != TaskFailure::None)
            require(allocationFailure && failure.kind == TaskFailure::Fault && failure.code == 2,
                    "array loop allocation failure changed its error channel");
          if (!allocationFailure && !fault)
            require(failure.kind == TaskFailure::None, "healthy array loop failed");
        }
        require(core->stats().peakBytes < 256 * 1024,
                "array loop retained storage proportional to iteration count");
        require(core->close(group) && core->drain() && core->release(group),
                "array loop group cleanup failed");
        const auto stats = core->stats();
        require(!stats.unfinished && !stats.records && !stats.groups && !stats.retainedBytes &&
                    !stats.retainedOwners && !stats.environmentBytes && !stats.recursiveBytes,
                "array loop leaked task storage");
        destination.settle();
        source.settle();
        require(!source.live() && !destination.live(), "array loop leaked observer storage");
      }
    }
    std::cout
        << "Array continuation scopes: bounded 2000-round task snapshots, workers 1/4, "
           "64 allocation failures, arithmetic faults, running cancellation and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
