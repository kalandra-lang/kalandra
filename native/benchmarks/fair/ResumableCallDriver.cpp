// Build this unchanged driver against each frozen installation's own headers.
#include "runtime/Tasks.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using namespace kalandra::runtime;
extern "C" const TaskEntry *kalandra_task_test_entry;
struct Result {
  Failure failure;
  std::int64_t value;
};
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  const auto count = std::strtoull(argv[1], nullptr, 10);
  const auto rounds = std::strtoull(argv[2], nullptr, 10);
  const auto expected = std::strtoll(argv[3], nullptr, 10);
  const auto &entry = *kalandra_task_test_entry;
  if (!count || !rounds || entry.packetBytes || entry.result->bytes != sizeof(Result))
    return 2;
  auto runner = ClosedTaskRunner::create();
  if (!runner)
    return 3;
  for (std::int64_t round = -2; round < std::int64_t(rounds); ++round) {
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < count; ++i) {
      Invocation receiver;
      Result result{};
      const auto failure =
          runner->run(entry, {entry.packetLayout, nullptr, 0}, *entry.result, receiver, &result);
      if (failure.kind != TaskFailure::None || result.failure.kind != TaskFailure::None ||
          result.value != expected || receiver.status())
        std::abort();
      checksum += result.value;
      entry.result->destroy(&result);
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"round\":%lld,\"ns\":%.9g,\"checksum\":%llu}\n", (long long)round, ns,
                (unsigned long long)checksum);
  }
}
