#include "../runtime/Tasks.h"
#include "kalandra/NumericType.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
using namespace kalandra;
using namespace kalandra::runtime;
struct Result {
  Failure failure;
  double value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void check(bool ok, const char *message) {
  if (!ok) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  const auto workers = unsigned(std::strtoul(argv[1], nullptr, 10));
  const auto expected = std::strtoull(argv[2], nullptr, 16);
  const int mode = std::atoi(argv[3]);
  const auto attempt = std::strtoull(argv[4], nullptr, 10);
  TaskLimits limits;
  limits.workers = workers;
  if (mode == 4)
    limits.bytes = 1;
  auto core = TaskCore::create(limits);
  check(bool(core), "core creation failed");
  if (mode == 3)
    core->failAllocationAt(attempt);
  auto *entry = kalandra_task_test_entry;
  check(entry && entry->result && entry->packetBytes == 0 &&
            entry->result->bytes == sizeof(Result) && entry->result->alignment == alignof(Result) &&
            entry->result->identity == 0x20000 + static_cast<unsigned>(NumericType::Float64),
        "wrong binary64 task ABI/layout");
  auto group = core->openRoot();
  check(bool(group), "root admission failed");
  Result out{}, again{};
  bool published = false;
  {
    Invocation destination;
    auto task = core->submit(group, *entry, Packet{entry->packetLayout, nullptr, 0, &destination});
    if (mode == 1 || mode == 2)
      check(core->cancel(group, mode == 2), "cancellation rejected");
    check(core->drain(), "root did not settle");
    auto failure = core->failure(group);
    if (failure.kind == TaskFailure::None && task)
      failure = core->failure(task);
    if (failure.kind == TaskFailure::None && task) {
      check(core->observe(task, *entry->result, destination, &out), "observation failed");
      failure = out.failure;
      if (failure.kind == TaskFailure::None) {
        published = true;
        check(std::bit_cast<std::uint64_t>(out.value) == expected, "result bits changed");
        check(core->observe(task, *entry->result, destination, &again) &&
                  again.failure.kind == TaskFailure::None &&
                  std::bit_cast<std::uint64_t>(again.value) == expected,
              "cached result bits changed");
        // Equal size/alignment is insufficient: float32/int64/uint64 layouts are distinct.
        for (auto identity :
             {std::uint64_t{1}, std::uint64_t{0x20000 + static_cast<unsigned>(NumericType::Int64)},
              std::uint64_t{0x20000 + static_cast<unsigned>(NumericType::UInt64)}}) {
          auto wrong = *entry->result;
          wrong.identity = identity;
          Invocation invalid;
          alignas(Result) std::array<std::byte, sizeof(Result)> bytes;
          bytes.fill(std::byte{0x5a});
          auto before = bytes;
          check(!core->observe(task, wrong, invalid, bytes.data()) && invalid.status() == 3 &&
                    bytes == before && !invalid.live(),
                "wrong same-size numeric layout published a result");
        }
      }
    }
    if (mode == 1 || mode == 2)
      check(failure.kind == (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
            "wrong cancellation kind");
    else if (mode == 4)
      check(failure.kind == TaskFailure::AdmissionRejected, "wrong admission kind");
    else if (mode == 3 && failure.kind != TaskFailure::None)
      check((failure.kind == TaskFailure::Fault && failure.code == 2) ||
                failure.kind == TaskFailure::AdmissionRejected,
            "wrong injected failure");
    else
      check(failure.kind == TaskFailure::None && published, "float64 worker failed");
    check(core->close(group) && core->drain() && core->release(group), "root release failed");
    const auto stats = core->stats();
    check(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished && !stats.records &&
              !stats.groups && !stats.ready && !stats.pending && !stats.maintenanceTasks &&
              !stats.externalPending,
          "scheduler state leaked");
    core.reset();
    destination.settle();
    check(!destination.live() && destination.allocations() == destination.releases(),
          "destination owners leaked");
  }
  if (published)
    check(std::bit_cast<std::uint64_t>(out.value) == expected,
          "scalar result changed after core/invocation destruction");
  std::cout << "float64 task workers " << workers << " mode " << mode << " allocation " << attempt
            << ": exact bits, cached layouts and settled lifetimes passed\n";
}
