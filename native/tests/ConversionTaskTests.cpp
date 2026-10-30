#include "../runtime/NumericStorage.h"
#include "../runtime/Tasks.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
using namespace kalandra;
using namespace kalandra::runtime;
#ifndef EXPECTED_FAULT
#define EXPECTED_FAULT 4
#endif
#ifndef DESTINATION_TYPE
#define DESTINATION_TYPE 9
#endif
constexpr auto destination = static_cast<NumericType>(DESTINATION_TYPE);
using Cell = NumericValue<destination>;
using Raw = std::conditional_t<
    sizeof(Cell) == 1, std::uint8_t,
    std::conditional_t<sizeof(Cell) == 2, std::uint16_t,
                       std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>>>;
struct Result {
  Failure failure;
  Cell value;
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
  auto workers = unsigned(std::strtoul(argv[1], nullptr, 10));
  auto expected = std::strtoull(argv[2], nullptr, 16);
  auto mode = std::atoi(argv[3]);
  auto attempt = std::strtoull(argv[4], nullptr, 10);
  TaskLimits limits;
  limits.workers = workers;
  if (mode == 4)
    limits.bytes = 1;
  auto core = TaskCore::create(limits);
  check(bool(core), "core creation failed");
  if (mode == 3)
    core->failAllocationAt(attempt);
  auto *entry = kalandra_task_test_entry;
  auto identity =
      destination == NumericType::Float32 ? 1 : 0x20000 + static_cast<unsigned>(destination);
  check(entry && entry->result && !entry->packetBytes && entry->result->bytes == sizeof(Result) &&
            entry->result->alignment == alignof(Result) && entry->result->identity == identity,
        "wrong conversion task layout");
  auto group = core->openRoot();
  check(bool(group), "root admission failed");
  Result out{}, again{};
  bool published = false;
  {
    Invocation context;
    auto task = core->submit(group, *entry, Packet{entry->packetLayout, nullptr, 0, &context});
    if (mode == 1 || mode == 2)
      check(core->cancel(group, mode == 2), "cancel rejected");
    check(core->drain(), "drain failed");
    auto failure = core->failure(group);
    if (failure.kind == TaskFailure::None && task)
      failure = core->failure(task);
    if (failure.kind == TaskFailure::None && task) {
      check(core->observe(task, *entry->result, context, &out), "observation failed");
      failure = out.failure;
      if (failure.kind == TaskFailure::None) {
        published = true;
        check(std::uint64_t(std::bit_cast<Raw>(out.value)) == expected,
              "wrong converted worker bits");
        check(core->observe(task, *entry->result, context, &again) &&
                  again.failure.kind == TaskFailure::None &&
                  std::uint64_t(std::bit_cast<Raw>(again.value)) == expected,
              "cached bits changed");
        auto wrong = *entry->result;
        wrong.identity ^= 0x100000;
        Invocation invalid;
        alignas(Result) std::array<std::byte, sizeof(Result)> bytes;
        bytes.fill(std::byte{0x5a});
        auto before = bytes;
        check(!core->observe(task, wrong, invalid, bytes.data()) && invalid.status() == 3 &&
                  bytes == before && !invalid.live(),
              "wrong layout published value");
      }
    }
    if (mode == 5)
      check(failure.kind == TaskFailure::Fault && failure.code == EXPECTED_FAULT &&
                failure.line == 2 && failure.column == 3 && !published,
            "wrong conversion arithmetic fault/location");
    else if (mode == 1 || mode == 2)
      check(failure.kind == (mode == 1 ? TaskFailure::Cancelled : TaskFailure::DeadlineExceeded),
            "wrong cancellation kind");
    else if (mode == 4)
      check(failure.kind == TaskFailure::AdmissionRejected, "wrong admission result");
    else if (mode == 3 && failure.kind != TaskFailure::None) {
      if (!((failure.kind == TaskFailure::Fault && failure.code == 2) ||
            failure.kind == TaskFailure::AdmissionRejected))
        std::cerr << "allocation failure kind " << unsigned(failure.kind) << " code "
                  << failure.code << " location " << failure.line << ":" << failure.column << "\n";
      check((failure.kind == TaskFailure::Fault && failure.code == 2) ||
                failure.kind == TaskFailure::AdmissionRejected,
            "wrong allocation failure");
    } else
      check(failure.kind == TaskFailure::None && published, "conversion worker failed");
    check(core->close(group) && core->drain() && core->release(group), "group release failed");
    auto stats = core->stats();
    check(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished && !stats.records &&
              !stats.groups && !stats.ready && !stats.pending && !stats.maintenanceTasks &&
              !stats.externalPending,
          "scheduler leaked");
    core.reset();
    context.settle();
    check(!context.live() && context.allocations() == context.releases(),
          "conversion owners leaked");
  }
  if (published)
    check(std::uint64_t(std::bit_cast<Raw>(out.value)) == expected,
          "value changed after owners destroyed");
  std::cout << "conversion task " << describeNumeric(destination)->name << " workers " << workers
            << " mode " << mode << " allocation " << attempt
            << ": exact bits, typed cached layouts and settled owners passed\n";
}
