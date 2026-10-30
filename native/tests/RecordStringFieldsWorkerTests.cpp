#include "../runtime/String.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset, size, stride;
};
struct Inner {
  StringDescriptor text;
  bool enabled;
};
struct Record {
  Inner data;
  StringDescriptor duplicate;
  Descriptor numbers;
};
struct Result {
  Failure failure;
  Record value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static std::string textFor(unsigned sample) {
  if (!sample)
    return "";
  return std::string(std::array<unsigned, 4>{0, 1, 257, 4097}[sample % 4], 'a') +
         std::string("é\0👩‍💻سلام", 22);
}
static void verify(Invocation &ctx, const Result &result, unsigned sample, bool nonempty = false) {
  require(result.failure.kind == TaskFailure::None && result.value.data.enabled,
          "record metadata changed");
  const auto text = textFor(nonempty ? 1 : sample);
  require(result.value.data.text.allocated == result.value.duplicate.allocated,
          "immutable String field backing was copied");
  for (const auto &d : {result.value.data.text, result.value.duplicate})
    require(validateString(ctx, d) && d.byteLength == std::int64_t(text.size()) &&
                (!d.byteLength || !std::memcmp(d.data, text.data(), text.size())),
            "record String bytes or ownership changed");
  const auto &numbers = result.value.numbers;
  auto lease = ctx.lease(numbers.allocated);
  require(lease && numbers.allocated == numbers.aligned && !numbers.offset && numbers.size == 8 &&
              numbers.stride == 1 && lease->bytes().size() == 64,
          "numeric sibling lost");
  for (unsigned i = 0; i < 8; ++i) {
    std::uint64_t value;
    std::memcpy(&value, lease->bytes().data() + i * 8, 8);
    require(value == UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i),
            "numeric sibling bits changed");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 4, "expected worker count, mode, allocation position");
    const auto mode = std::atoi(argv[2]);
    TaskLimits limits;
    limits.workers = std::atoi(argv[1]);
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "scheduler creation failed");
    if (mode == 3)
      core->failAllocationAt(std::strtoull(argv[3], nullptr, 10));
    const auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->packetAlignment == alignof(Result) &&
                entry->result->bytes == sizeof(Result) && entry->result->identity == 0x50000,
            "record String layout mismatch");
    Invocation survivor;
    Result surviving{};
    unsigned last = 0;
    for (unsigned sample = 0; sample < (mode == 0 ? 12u : 1u); ++sample) {
      Invocation source, destination;
      const auto text = textFor(mode == 0 ? sample : 1);
      StringDescriptor descriptor;
      kalandra_string_literal_v1(&source, &descriptor, text.data(), text.size());
      auto *numbers = static_cast<std::byte *>(source.allocateRecord(64));
      require(numbers && !source.status(), "source allocation failed");
      for (unsigned i = 0; i < 8; ++i) {
        auto raw = UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i);
        std::memcpy(numbers + i * 8, &raw, 8);
      }
      Result packet{{}, {{descriptor, true}, descriptor, {numbers, numbers, 0, 8, 1}}};
      if (mode == 5)
        const_cast<char *>(descriptor.data)[descriptor.byteLength - 1] = char(0xff);
      if (mode == 6)
        ++packet.value.duplicate.byteLength;
      if (mode == 7)
        packet.value.numbers.size = 9;
      if (mode == 8)
        ++packet.value.data.text.data;
      if (mode == 9)
        packet.value.duplicate.allocated = reinterpret_cast<void *>(1);
      if (mode == 10)
        packet.value.duplicate.byteLength = 0;
      if (mode == 11)
        packet.value.duplicate.byteLength = -1;
      alignas(Result) std::array<std::byte, sizeof(Result)> guarded;
      guarded.fill(std::byte{0x5a});
      const auto before = guarded;
      if (mode >= 5) {
        Invocation rejected;
        require(!entry->result->copy(rejected, guarded.data(), source, &packet) &&
                    rejected.status() == 3 && guarded == before,
                "invalid String record published");
        rejected.settle();
        require(rejected.allocations() == rejected.releases() && !rejected.live(),
                "partial String record validation leaked");
      }
      if (mode == 0) {
        for (unsigned attempt = 1; attempt <= (sample ? 3u : 1u); ++attempt) {
          Invocation rejected;
          rejected.failAllocationAt(attempt);
          require(!entry->result->copy(rejected, guarded.data(), source, &packet) &&
                      rejected.status() == 2 && guarded == before,
                  "partial owner failure published");
          rejected.settle();
          require(!rejected.live() && rejected.allocations() == rejected.releases(),
                  "failed String record copy leaked");
        }
        Invocation stopped;
        std::atomic<bool> stop = true;
        stopped.bindCancellation(&stop);
        require(!entry->result->copy(stopped, guarded.data(), source, &packet) && guarded == before,
                "cancelled String record copy published");
        stopped.settle();
        require(!stopped.live(), "cancelled String record copy leaked");
      }
      auto group = core->openRoot();
      auto task =
          core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
      source.settle();
      packet = {};
      if (mode == 1 || mode == 2)
        require(core->cancel(group, mode == 2), "cancel failed");
      require(core->drain(), "drain failed");
      auto failure = core->failure(group);
      if (failure.kind == TaskFailure::None && task)
        failure = core->failure(task);
      bool success = false;
      if (failure.kind == TaskFailure::None && task) {
        Result result{};
        require(core->observe(task, *entry->result, destination, &result), "observe failed");
        failure = result.failure;
        success = failure.kind == TaskFailure::None;
        if (success) {
          verify(destination, result, sample, mode != 0);
          result = {};
          require(core->observe(task, *entry->result, destination, &result),
                  "cached observe failed");
          verify(destination, result, sample, mode != 0);
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving), "survivor failed");
            last = sample;
          }
        }
      }
      if (mode == 1 || mode == 2)
        require(!success && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                       : TaskFailure::DeadlineExceeded),
                "cancelled String record published");
      else if (mode == 4)
        require(!success && failure.kind == TaskFailure::AdmissionRejected, "admission accepted");
      else if (mode >= 5)
        require(!success && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid packet accepted");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!success && (failure.kind == TaskFailure::AdmissionRejected ||
                             (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation failure changed");
      else
        require(success, "String record worker failed");
      require(core->close(group) && core->drain() && core->release(group), "group cleanup failed");
      const auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                  !stats.unfinished && !stats.ready && !stats.pending && !stats.externalPending,
              "String record scheduler leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases() &&
                  source.allocations() == source.releases(),
              "String record owners leaked");
    }
    core.reset();
    if (mode == 0)
      verify(survivor, surviving, last);
    survivor.settle();
    require(!survivor.live() && survivor.allocations() == survivor.releases(), "survivor leaked");
    std::cout << "String record workers " << limits.workers << " mode " << mode
              << ": exact text, shared backing, nested retention and settlement passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
