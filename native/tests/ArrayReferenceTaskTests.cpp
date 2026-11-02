#include "../runtime/Tensor.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Array = CpuTensor<element, 1>;
struct PacketValue {
  Failure failure;
  Array input;
};
struct Aliases {
  Array original, result;
};
struct Result {
  Failure failure;
  Aliases value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(PacketValue) &&
                entry->result->bytes == sizeof(Result),
            "reference task ABI mismatch");
    unsigned successes = 0;
    for (unsigned workers : {1u, 4u})
      for (unsigned attempt = 0; attempt < 25; ++attempt) {
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        require(bool(core), "reference task runner creation failed");
        if (attempt > 1)
          core->failAllocationAt(attempt - 1);
        auto group = core->openRoot();
        require(bool(group), "reference root group rejected");
        Invocation source, destination;
        auto *storage = static_cast<Cell *>(source.allocate(3 * sizeof(Cell)));
        require(storage, "reference source allocation failed");
        for (unsigned i = 0; i < 3; ++i)
          storage[i] = static_cast<Cell>(i + 1);
        PacketValue packet{{}, {storage, storage, 0, {3}, {1}}};
        auto task =
            core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
        source.settle();
        if (attempt == 1)
          require(core->cancel(group), "reference cancellation rejected");
        require(core->drain(), "reference task did not settle");
        auto failure = core->failure(group);
        if (task && failure.kind == TaskFailure::None)
          failure = core->failure(task);
        Result output{};
        if (failure.kind == TaskFailure::None && task) {
          const bool observed = core->observe(task, *entry->result, destination, &output);
          require(observed, "reference result observation failed");
          if (output.failure.kind == TaskFailure::None) {
            require(output.value.original.allocated == storage &&
                        output.value.result.allocated == storage,
                    "task transport cloned an array alias");
            ++successes;
          } else
            require(attempt > 1 &&
                        ((output.failure.kind == TaskFailure::Fault && output.failure.code == 2) ||
                         output.failure.kind == TaskFailure::AdmissionRejected) &&
                        !output.value.original.allocated && !output.value.result.allocated,
                    "allocation failure published partial aliases or changed its error channel");
        } else
          require(failure.kind == TaskFailure::Fault || failure.kind == TaskFailure::Cancelled ||
                      failure.kind == TaskFailure::AdmissionRejected,
                  "reference task failure precedence changed");
        require(core->close(group) && core->drain() && core->release(group),
                "reference task group cleanup failed");
        auto stats = core->stats();
        require(!stats.records && !stats.groups && !stats.retainedOwners && !stats.retainedBytes,
                "reference task retained scheduler resources");
        if (output.value.original.allocated) {
          require(output.value.result.aligned[2] == static_cast<Cell>(3),
                  "result alias died with the task group");
          output.value.original.aligned[0] = static_cast<Cell>(42);
          require(output.value.result.aligned[0] == static_cast<Cell>(42),
                  "settled task result lost its shared backing");
        }
        destination.settle();
        require(!source.live() && !destination.live(),
                "reference handoff leaked invocation owners");
      }
    require(successes >= 2, "reference task has no healthy controls");
    for (unsigned workers : {1u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto runner = ClosedTaskRunner::create(limits);
      require(bool(runner), "closed reference runner creation failed");
      for (unsigned attempt = 0; attempt < 13; ++attempt) {
        runner->failAllocationAt(attempt);
        Invocation source, destination;
        auto *storage = static_cast<Cell *>(source.allocate(3 * sizeof(Cell)));
        require(storage, "closed reference source allocation failed");
        for (unsigned i = 0; i < 3; ++i)
          storage[i] = static_cast<Cell>(i + 1);
        PacketValue packet{{}, {storage, storage, 0, {3}, {1}}};
        Result output{};
        auto failure = runner->run(*entry, {entry->packetLayout, &packet, sizeof(packet), &source},
                                   *entry->result, destination, &output);
        if (failure.kind == TaskFailure::None)
          failure = output.failure;
        source.settle();
        if (failure.kind == TaskFailure::None) {
          require(output.value.original.allocated == storage &&
                      output.value.result.allocated == storage &&
                      output.value.result.aligned[2] == static_cast<Cell>(3),
                  "closed runner cloned or lost a returned alias");
          output.value.original.aligned[0] = static_cast<Cell>(42);
          require(output.value.result.aligned[0] == static_cast<Cell>(42),
                  "closed runner result lost shared storage");
        } else
          require(attempt &&
                      ((failure.kind == TaskFailure::Fault && failure.code == 2) ||
                       failure.kind == TaskFailure::AdmissionRejected) &&
                      !output.value.original.allocated && !output.value.result.allocated,
                  "closed runner failure changed channel or published partial aliases");
        const auto stats = runner->stats();
        require(!stats.records && !stats.groups && !stats.retainedOwners && !stats.retainedBytes,
                "closed runner retained reference resources");
        destination.settle();
        require(!source.live() && !destination.live(), "closed reference leaked owners");
      }
    }
    std::cout << "Task array identity: workers 1/4, settlement, cancellation and allocation faults "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
