#include "../runtime/Tasks.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace kalandra::runtime;
extern "C" const TaskEntry *const kalandra_task_test_entry;
struct Result {
  Failure failure;
  std::int64_t value;
};
static void require(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
int main() {
  try {
    auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) && entry->result->bytes == sizeof(Result),
            "loop task descriptor mismatch");
    for (unsigned workers : {1u, 4u})
      for (bool cancel : {false, true}) {
        TaskLimits limits;
        limits.workers = workers;
        limits.records = 16;
        limits.groups = 8;
        limits.ready = 8;
        limits.inFlight = 8;
        auto core = TaskCore::create(limits);
        require(bool(core), "task core creation failed");
        auto group = core->openRoot();
        Invocation source;
        Result packet{{}, cancel ? INT64_MAX : 2000};
        auto task =
            core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
        require(bool(task), "loop task submission failed");
        bool drained = false;
        if (cancel) {
          std::jthread drainer(
              [&] { drained = core->drain(std::numeric_limits<std::size_t>::max()); });
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          while (core->stats().resumes < 20 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
          const bool ran = core->stats().resumes >= 20;
          const bool requested = core->cancel(group);
          drainer.join();
          require(ran && requested && drained,
                  "running continuation loop failed to cancel and drain");
          require(core->failure(group).kind == TaskFailure::Cancelled,
                  "continuation cancellation kind changed");
        } else {
          require(core->drain(), "bounded task loop did not drain");
          require(core->failure(task).kind == TaskFailure::None,
                  "task iteration exhausted scheduler records");
          Result result{};
          require(core->observe(task, *entry->result, source, &result) &&
                      result.failure.kind == TaskFailure::None && result.value == 1999000,
                  "task loop lost snapshots or mutable state across suspension");
        }
        require(core->close(group) && core->drain() && core->release(group),
                "task loop cleanup failed");
        auto stats = core->stats();
        require(!stats.unfinished && !stats.records && !stats.groups && !stats.retainedBytes &&
                    !stats.retainedOwners && !stats.externalPending,
                "loop retained scheduler resources");
      }
    std::cout << "Continuation loops: 2000 iterations with 16 records/8 groups, snapshots across "
                 "awaits, running cancellation and full cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
