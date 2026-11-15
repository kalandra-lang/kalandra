#include "../runtime/Tasks.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <thread>
using namespace kalandra::runtime;
extern "C" const TaskEntry *kalandra_task_test_entry;
static void check(bool value, std::source_location site = std::source_location::current()) {
  if (!value) {
    std::fprintf(stderr, "cancellation check failed at line %u\n", site.line());
    std::abort();
  }
}
int main() {
  for (unsigned workers : {1u, 4u})
    for (bool deadline : {false, true})
      for (bool before : {false, true}) {
        TaskLimits limits;
        limits.workers = workers;
        limits.records = 8;
        limits.groups = 8;
        limits.bytes = 1024 * 1024;
        auto core = TaskCore::create(limits);
        check(bool(core));
        const auto root = core->openRoot();
        if (before)
          check(core->cancel(root, deadline));
        const auto &entry = *kalandra_task_test_entry;
        check(entry.packetBytes == 0);
        const auto task = core->submit(root, entry, {entry.packetLayout, nullptr, 0});
        check(bool(task) && core->close(root));
        std::jthread stop;
        if (!before)
          stop = std::jthread([&] {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!core->stats().peakRunning && std::chrono::steady_clock::now() < until)
              std::this_thread::yield();
            check(core->stats().peakRunning != 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            check(core->cancel(root, deadline));
          });
        const auto start = std::chrono::steady_clock::now();
        check(core->drainToCompletion());
        if (stop.joinable())
          stop.join();
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
        check(core->failure(root).kind ==
              (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
        // Admission into an already expired group inherits DeadlineExceeded.
        // A running task observes its cancellation token; the owning group
        // retains the deadline cause in both cases.
        check(core->failure(task).kind ==
              (before && deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
        check(core->release(root));
        const auto stats = core->stats();
        check(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.callBytes);
      }
  std::puts("PASS: fused traversal cancellation/deadline, before/during, one/four workers");
}
