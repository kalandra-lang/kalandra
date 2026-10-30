#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *data;
  std::int64_t offset, size, stride;
};
struct Error {
  std::int64_t tag;
  StringDescriptor reason;
  Descriptor numbers;
  std::array<Failure, 4> details;
};
struct Value {
  std::int64_t number;
};
struct Result {
  Failure failure;
  Value value;
  Error error;
};
struct Argument {
  Failure failure;
  Failure error;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void verify(Invocation &context, const Result &result, bool application, Failure expected) {
  require(result.failure.kind == TaskFailure::None, "typed mixed result became execution failure");
  if (application) {
    require(result.error.tag == 1, "application family changed");
    const auto &text = result.error.reason;
    require(validateString(context, text) && text.byteLength == 6 &&
                !std::memcmp(text.data, "é🙂", 6),
            "application text changed");
    const auto &numbers = result.error.numbers;
    auto owner = context.lease(numbers.allocated);
    require(owner && numbers.allocated == numbers.data && numbers.offset == 0 &&
                numbers.size == 2 && numbers.stride == 1 && owner->bytes().size() == 16,
            "application array changed");
    std::array<std::int64_t, 2> values{};
    std::memcpy(values.data(), owner->bytes().data(), 16);
    require(values[0] == 20 && values[1] == 22, "application values changed");
  } else {
    const auto kind = static_cast<unsigned>(expected.kind);
    require(result.error.tag == kind + 1, "TaskError variant changed");
    const auto &actual = result.error.details[kind - 1];
    require(actual.kind == expected.kind && actual.code == expected.code &&
                actual.line == expected.line && actual.column == expected.column,
            "TaskError details changed");
    require(!context.live(), "TaskError unexpectedly retained inactive application owners");
  }
}
static void run(bool application, unsigned workers, Failure expected) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  require(bool(core), "scheduler creation failed");
  auto *entry = kalandra_task_test_entry;
  require(entry && entry->result->bytes == sizeof(Result) &&
              entry->packetBytes == (application ? 0 : sizeof(Argument)),
          "mixed result/packet layout mismatch");
  Invocation source, survivor;
  Argument argument{{}, expected};
  auto group = core->openRoot();
  auto task = core->submit(
      group, *entry,
      {entry->packetLayout, application ? nullptr : &argument, entry->packetBytes, &source});
  require(task && core->close(group) && core->drain(), "mixed task did not settle");
  require(core->failure(task).kind == TaskFailure::None,
          "expected mixed result became task failure");
  for (unsigned attempt = 1; attempt <= 2; ++attempt) {
    Invocation rejected;
    rejected.failAllocationAt(attempt);
    alignas(Result) std::array<std::byte, sizeof(Result)> output;
    output.fill(std::byte{0x5a});
    const auto before = output;
    const auto ok = core->observe(task, *entry->result, rejected, output.data());
    require(ok != application, "selected payload retention did not match its ownership");
    if (application)
      require(output == before, "failed observation published partial output");
    rejected.settle();
    require(!rejected.live() && rejected.allocations() == rejected.releases(),
            "failed observation leaked");
  }
  Result result{};
  require(core->observe(task, *entry->result, survivor, &result), "mixed observation failed");
  verify(survivor, result, application, expected);
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto bad = result;
    if (mode == 0)
      bad.error.tag = 999;
    else if (application) {
      if (mode == 1)
        bad.error.reason.byteLength = -1;
      else
        bad.error.numbers.stride = 2;
    } else
      bad.error.details[static_cast<unsigned>(expected.kind) - 1].kind =
          mode == 1
              ? TaskFailure::None
              : (expected.kind == TaskFailure::Fault ? TaskFailure::Cancelled : TaskFailure::Fault);
    Invocation rejected;
    alignas(Result) std::array<std::byte, sizeof(Result)> output;
    output.fill(std::byte{0x5a});
    const auto before = output;
    require(!entry->result->copy(rejected, output.data(), survivor, &bad) && output == before,
            "malformed mixed payload published output");
    rejected.settle();
    require(!rejected.live() && rejected.allocations() == rejected.releases(),
            "malformed retention leaked");
  }
  auto poison = result;
  if (application)
    for (auto &failure : poison.error.details)
      failure.kind = static_cast<TaskFailure>(99);
  else {
    poison.error.reason.byteLength = -1;
    poison.error.numbers.stride = -1;
  }
  Invocation selected;
  Result observed{};
  require(entry->result->copy(selected, &observed, survivor, &poison),
          "inactive mixed arm was interpreted");
  verify(selected, observed, application, expected);
  selected.settle();
  require(!selected.live() && selected.allocations() == selected.releases(),
          "inactive-arm isolation leaked");
  Result again{};
  require(core->observe(task, *entry->result, survivor, &again), "cached mixed observation failed");
  if (application)
    require(result.error.reason.allocated == again.error.reason.allocated &&
                result.error.numbers.allocated == again.error.numbers.allocated,
            "mixed backing unexpectedly copied");
  require(core->release(group), "mixed group release failed");
  auto stats = core->stats();
  require(!stats.records && !stats.groups && !stats.retainedOwners && !stats.retainedBytes,
          "mixed scheduler ownership leaked");
  core.reset();
  verify(survivor, result, application, expected);
  verify(survivor, again, application, expected);
  survivor.settle();
  require(!survivor.live() && survivor.allocations() == survivor.releases(),
          "surviving mixed owners leaked");
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected worker count");
    const auto workers = std::stoul(argv[1]);
#ifdef KALANDRA_MIXED_APPLICATION_PROBE
    run(true, workers, {});
#else
    for (auto code :
         {std::int64_t(1), std::int64_t(2), std::int64_t(4), std::int64_t(5), std::int64_t(99)})
      run(false, workers, {TaskFailure::Fault, code, 17, 9});
    for (auto kind :
         {TaskFailure::Cancelled, TaskFailure::DeadlineExceeded, TaskFailure::AdmissionRejected})
      run(false, workers, {kind, 0, 23, 11});
#endif
    std::cout << "Mixed results: original TaskError details, active ownership, cached "
                 "observations, rejected metadata and scheduler teardown passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
