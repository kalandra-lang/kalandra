#include "../runtime/Tasks.h"
#include <cstdio>
#include <cstdlib>
using namespace kalandra::runtime;
extern "C" const TaskEntry *kalandra_task_test_entry;
struct Answer {
  Failure failure;
  std::int64_t value;
};
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  const auto expected = std::strtoll(argv[1], nullptr, 10);
  TaskLimits limits;
  limits.workers = unsigned(std::strtoul(argv[2], nullptr, 10));
  limits.records = 8;
  limits.groups = 8;
  limits.ready = 2;
  limits.bytes = 1024 * 1024;
  const auto &entry = *kalandra_task_test_entry;
  if (entry.packetBytes || entry.result->bytes != sizeof(Answer))
    return 2;
  auto core = TaskCore::create(limits);
  if (!core)
    return 3;
  auto group = core->openRoot();
  auto task = core->submit(group, entry, {entry.packetLayout, nullptr, 0});
  if (!task || !core->close(group) || !core->drainToCompletion())
    return 4;
  auto failure = core->failure(task);
  if (failure.kind != TaskFailure::None) {
    std::fprintf(stderr, "task failure %u code %lld at %lld:%lld\n", unsigned(failure.kind),
                 static_cast<long long>(failure.code), static_cast<long long>(failure.line),
                 static_cast<long long>(failure.column));
    return 5;
  }
  Invocation receiver;
  Answer answer{};
  if (!core->observe(task, *entry.result, receiver, &answer) ||
      answer.failure.kind != TaskFailure::None || answer.value != expected || receiver.poll()) {
    std::fprintf(stderr, "incorrect result %lld, expected %lld\n",
                 static_cast<long long>(answer.value), static_cast<long long>(expected));
    return 6;
  }
  if (!core->release(group))
    return 7;
  const auto stats = core->stats();
  if (stats.retainedBytes || stats.retainedOwners || stats.records || stats.callBytes)
    return 8;
  std::puts("PASS");
}
