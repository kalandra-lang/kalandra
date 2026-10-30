#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset, size, stride;
};
struct Inner {
  Descriptor flags;
  bool enabled;
};
struct Record {
  Inner data;
  Descriptor duplicate, numbers;
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
static unsigned char bit(unsigned sample, std::size_t i) { return (sample + i * 17 + i / 7) & 1; }
static std::uint64_t raw(unsigned sample, unsigned i) {
  return UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i);
}
static void verify(Invocation &context, const Result &result, unsigned sample, std::size_t count) {
  require(result.failure.kind == TaskFailure::None && result.value.data.enabled,
          "record metadata changed");
  require(result.value.data.flags.allocated == result.value.duplicate.allocated,
          "Bool field unexpectedly copied its shared immutable backing");
  for (const auto &d : {result.value.data.flags, result.value.duplicate}) {
    require(d.allocated == d.aligned && !d.offset && d.size == std::int64_t(count) &&
                d.stride == (count ? 1 : 0),
            "Bool field descriptor changed");
    if (!count) {
      require(!d.allocated, "empty Bool field allocated");
      continue;
    }
    auto lease = context.lease(d.allocated);
    require(lease && lease->bytes().size() == count, "Bool field storage lost");
    for (std::size_t i = 0; i < count; ++i)
      require(std::to_integer<unsigned char>(lease->bytes()[i]) == bit(sample, i),
              "Bool field lane changed");
  }
  const auto &d = result.value.numbers;
  auto lease = context.lease(d.allocated);
  require(lease && d.allocated == d.aligned && !d.offset && d.size == 8 && d.stride == 1 &&
              lease->bytes().size() == 64,
          "numeric sibling lost");
  for (unsigned i = 0; i < 8; ++i) {
    std::uint64_t value;
    std::memcpy(&value, lease->bytes().data() + i * 8, 8);
    require(value == raw(sample, i), "numeric sibling bits changed");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 4, "expected workers, mode, failure position");
    auto mode = std::atoi(argv[2]);
    TaskLimits limits;
    limits.workers = std::atoi(argv[1]);
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "core failed");
    if (mode == 3)
      core->failAllocationAt(std::strtoull(argv[3], nullptr, 10));
    auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->result->bytes == sizeof(Result) && entry->result->identity == 0x50000,
            "record Bool layout mismatch");
    Invocation survivor;
    Result surviving{};
    unsigned finalSample = 0;
    std::size_t finalCount = 0;
    for (unsigned sample = 0; sample < (mode == 0 ? 20u : 1u); ++sample) {
      auto count = mode == 0 ? std::array<std::size_t, 4>{0, 1, 257, 4097}[sample % 4] : 7;
      Invocation source, destination;
      auto *flags = count ? static_cast<unsigned char *>(source.allocateRecord(count)) : nullptr;
      auto *numbers = static_cast<std::byte *>(source.allocateRecord(64));
      require((!count || flags) && numbers, "source allocation failed");
      for (std::size_t i = 0; i < count; ++i)
        flags[i] = bit(sample, i);
      for (unsigned i = 0; i < 8; ++i) {
        auto n = raw(sample, i);
        std::memcpy(numbers + i * 8, &n, 8);
      }
      Descriptor d{flags, flags, 0, std::int64_t(count), count ? 1 : 0};
      Result packet{{}, {{d, true}, d, {numbers, numbers, 0, 8, 1}}};
      if (mode == 5)
        flags[count - 1] = 2;
      if (mode == 6)
        packet.value.duplicate.stride = -1;
      if (mode == 7)
        packet.value.numbers.size = 9;
      if (mode == 8)
        packet.value.data.flags.offset = 1;
      if (mode == 0 && count) {
        Invocation stopped;
        std::atomic<bool> cancel = true;
        std::atomic<std::uint64_t> polls = 0;
        stopped.bindCancellation(&cancel);
        stopped.watchPolls(&polls);
        alignas(Result) std::array<std::byte, sizeof(Result)> out;
        out.fill(std::byte{0x5a});
        const auto before = out;
        require(!entry->result->copy(stopped, out.data(), source, &packet) && out == before &&
                    polls == 1,
                "cancelled nested Bool copy published");
        stopped.settle();
        require(!stopped.live(), "cancelled copy leaked");
      }
      if (mode >= 5) {
        Invocation rejected;
        alignas(Result) std::array<std::byte, sizeof(Result)> out;
        out.fill(std::byte{0x5a});
        const auto before = out;
        require(!entry->result->copy(rejected, out.data(), source, &packet) && out == before &&
                    rejected.status() == 3,
                "invalid field copy published output");
        rejected.settle();
        require(!rejected.live() && rejected.allocations() == rejected.releases(),
                "partial field validation leaked");
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
      bool published = false;
      if (failure.kind == TaskFailure::None && task) {
        if (mode == 0 && count)
          for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            Invocation rejected;
            rejected.failAllocationAt(attempt);
            alignas(Result) std::array<std::byte, sizeof(Result)> out;
            out.fill(std::byte{0x5a});
            const auto before = out;
            require(!core->observe(task, *entry->result, rejected, out.data()) && out == before &&
                        rejected.status() == 2,
                    "failed field observe published");
            rejected.settle();
            require(!rejected.live() && rejected.allocations() == rejected.releases(),
                    "failed field observe leaked");
          }
        struct {
          std::uint64_t before = 17;
          Result value{};
          std::uint64_t after = 23;
        } guarded;
        require(core->observe(task, *entry->result, destination, &guarded.value), "observe failed");
        failure = guarded.value.failure;
        if (failure.kind == TaskFailure::None) {
          verify(destination, guarded.value, sample, count);
          guarded.value = {};
          require(core->observe(task, *entry->result, destination, &guarded.value),
                  "cached observe failed");
          verify(destination, guarded.value, sample, count);
          require(guarded.before == 17 && guarded.after == 23, "record copy damaged canaries");
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving),
                    "survivor observe failed");
            finalSample = sample;
            finalCount = count;
          }
          published = true;
        }
      }
      if (mode == 1 || mode == 2)
        require(!published && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                         : TaskFailure::DeadlineExceeded),
                "cancelled record published");
      else if (mode == 4)
        require(!published && failure.kind == TaskFailure::AdmissionRejected,
                "record admission accepted");
      else if (mode >= 5)
        require(!published && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid record field accepted");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!published && (failure.kind == TaskFailure::AdmissionRejected ||
                               (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation fault changed");
      else
        require(published, "record Bool worker failed");
      require(core->close(group) && core->drain() && core->release(group), "group cleanup failed");
      auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                  !stats.unfinished,
              "scheduler leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases(),
              "record Bool owners leaked");
    }
    core.reset();
    if (mode == 0)
      verify(survivor, surviving, finalSample, finalCount);
    survivor.settle();
    require(!survivor.live() && survivor.allocations() == survivor.releases(), "survivor leaked");
    std::cout << "record Bool fields workers " << limits.workers << " mode " << mode
              << ": exact values, shared backing and cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
