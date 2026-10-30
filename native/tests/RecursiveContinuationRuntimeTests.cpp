#include "../runtime/Tasks.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
extern "C" const TaskEntry *kalandra_task_test_entry;
struct Result {
  Failure failure;
  std::int64_t value;
};
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc >= 2 && argc <= 4, "expected result, optional mode and allocation attempt");
    const auto expected = std::stoll(argv[1]);
    const auto mode = argc > 2 ? std::stoi(argv[2]) : 0;
    const auto attempt = argc > 3 ? std::stoull(argv[3]) : 0;
    const auto &entry = *kalandra_task_test_entry;
    require(!entry.packetBytes && entry.result && entry.result->bytes == sizeof(Result),
            "unexpected planned entry layout");
    for (unsigned workers : {1u, 2u, 4u}) {
      std::size_t admitted = 0, peak = 0;
      for (unsigned boundary = 0; boundary < (mode == 7 ? 4u : 1u); ++boundary) {
        TaskLimits limits;
        limits.workers = workers;
        if (mode == 4)
          limits.bytes = 1;
        if (mode == 8)
          limits.inFlight = 2;
        if (boundary)
          limits.bytes = boundary == 1 ? admitted : peak - (boundary == 2 ? 1 : 0);
        auto core = TaskCore::create(limits);
        require(bool(core), "planned scheduler unavailable");
        Invocation source, observer;
        auto group = core->openRoot();
        require(bool(group), "planned source root unavailable");
        if (mode == 5)
          core->failAllocationAt(attempt);
        auto task = core->submit(group, entry, {entry.packetLayout, nullptr, 0, &source});
        source.settle();
        require(bool(task), "planned source task missing");
        if (!boundary)
          admitted = core->stats().retainedBytes;
        if (mode == 3) {
          require(core->step() && !core->settled(task),
                  "source task did not suspend before cancel");
          require(core->stats().unfinished > 1, "source cancellation did not reach child work");
        }
        if (mode == 1 || mode == 2 || mode == 3)
          require(core->cancel(group, mode == 2), "source task cancellation rejected");
        require(core->drain(), "planned source task did not settle");
        if (!boundary)
          peak = core->stats().peakBytes;
        require(core->stats().peakBytes <= limits.bytes, "source task escaped byte cap");
        auto failure = core->failure(group);
        if (failure.kind == TaskFailure::None)
          failure = core->failure(task);
        Result first{{}, -1}, repeated{{}, -1};
        bool published = false;
        if (failure.kind == TaskFailure::None) {
          require(core->observe(task, *entry.result, observer, &first) &&
                      core->observe(task, *entry.result, observer, &repeated),
                  "planned cached result unavailable");
          failure = first.failure;
          published = failure.kind == TaskFailure::None;
        } else {
          require(!core->observe(task, *entry.result, observer, &first) && first.value == -1 &&
                      !observer.live(),
                  "failed source task published observer output");
        }
        if (mode == 1 || mode == 2 || mode == 3)
          require(!published && failure.kind == (mode == 2 ? TaskFailure::DeadlineExceeded
                                                           : TaskFailure::Cancelled),
                  "source cancellation/deadline changed kind");
        else if (mode == 4 || mode == 8)
          require(!published && failure.kind == TaskFailure::AdmissionRejected,
                  "source admission cap changed kind");
        else if (mode == 5 || mode == 6 || (mode == 7 && (boundary == 1 || boundary == 2))) {
          if (published)
            require(mode != 7, "source task ignored exhausted memory budget");
          else
            require((failure.kind == TaskFailure::Fault && failure.code == 2) ||
                        failure.kind == TaskFailure::AdmissionRejected,
                    "source allocation failure changed kind");
        } else
          require(published, "planned source task failed");
        require(core->close(group) && core->drain() && core->release(group),
                "planned source task cleanup failed");
        const auto stats = core->stats();
        require(!stats.records && !stats.groups && !stats.retainedOwners && !stats.retainedBytes &&
                    !stats.recursiveBytes && !stats.environmentBytes && !stats.unfinished &&
                    !stats.pending && !stats.ready && !stats.maintenanceTasks &&
                    !stats.externalPending,
                "planned source task leaked scheduler storage");
        core.reset();
        if (published)
          require(first.value == expected && repeated.value == expected &&
                      repeated.failure.kind == TaskFailure::None,
                  "planned source result changed");
        observer.settle();
        require(!observer.live() && observer.allocations() == observer.releases(),
                "planned source observer leaked");
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
