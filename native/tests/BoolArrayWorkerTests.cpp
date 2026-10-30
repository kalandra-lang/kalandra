#include "../runtime/CollectionDisplay.h"
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
struct Result {
  Failure failure;
  Descriptor value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static unsigned char bit(unsigned sample, std::size_t i) { return (sample + i * 17 + (i / 7)) & 1; }
static void verify(Invocation &context, const Result &result, unsigned sample, std::size_t count) {
  require(result.failure.kind == TaskFailure::None && result.value.size == std::int64_t(count),
          "Bool result metadata changed");
  require(!result.value.offset && result.value.stride == (count ? 1 : 0) &&
              result.value.allocated == result.value.aligned,
          "Bool layout changed");
  if (!count) {
    require(!result.value.allocated, "empty Bool array allocated");
    return;
  }
  auto owner = context.lease(result.value.allocated);
  require(owner && owner->bytes().size() == count, "Bool backing lost or not byte-sized");
  for (std::size_t i = 0; i < count; ++i)
    require(std::to_integer<unsigned char>(owner->bytes()[i]) == bit(sample, i),
            "Bool lane changed");
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
                entry->result->bytes == sizeof(Result) && entry->result->identity == 0x70000,
            "Bool task layout aliases another type");
    Invocation survivor;
    Result surviving{};
    unsigned finalSample = 0;
    std::size_t finalCount = 0;
    for (unsigned sample = 0; sample < (mode == 0 ? 20u : 1u); ++sample) {
      auto count = mode == 0 ? std::array<std::size_t, 4>{0, 1, 257, 4097}[sample % 4] : 7;
      Invocation source, destination;
      auto *storage =
          count ? static_cast<unsigned char *>(kalandra_record_alloc_v1(&source, count)) : nullptr;
      require(!count || storage, "source allocation failed");
      for (std::size_t i = 0; i < count; ++i)
        storage[i] = bit(sample, i);
      Result packet{{}, {storage, storage, 0, std::int64_t(count), count ? 1 : 0}};
      if (mode == 5)
        storage[0] = 2;
      if (mode == 6)
        packet.value.offset = 1;
      if (mode == 7)
        packet.value.stride = -1;
      if (mode == 8)
        packet.value.size = INT64_MAX;
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
                "cancelled Bool retention published output");
        stopped.settle();
        require(!stopped.live(), "cancel retain leaked");
      }
      auto group = core->openRoot();
      auto task =
          core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
      packet = {};
      source.settle();
      if (mode == 1 || mode == 2)
        require(core->cancel(group, mode == 2), "cancel failed");
      require(core->drain(), "drain failed");
      auto failure = core->failure(group);
      if (failure.kind == TaskFailure::None && task)
        failure = core->failure(task);
      bool published = false;
      if (failure.kind == TaskFailure::None && task) {
        if (mode == 0 && count) {
          Invocation rejected;
          rejected.failAllocationAt(1);
          alignas(Result) std::array<std::byte, sizeof(Result)> out;
          out.fill(std::byte{0x5a});
          const auto before = out;
          require(!core->observe(task, *entry->result, rejected, out.data()) &&
                      rejected.status() == 2 && out == before,
                  "failed Bool observation published output");
          rejected.settle();
          require(!rejected.live() && rejected.allocations() == rejected.releases(),
                  "failed observe leaked");
        }
        Result result{};
        require(core->observe(task, *entry->result, destination, &result), "observe failed");
        failure = result.failure;
        if (failure.kind == TaskFailure::None) {
          verify(destination, result, sample, count);
          result = {};
          require(core->observe(task, *entry->result, destination, &result),
                  "cached observe failed");
          verify(destination, result, sample, count);
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving), "survivor failed");
            finalSample = sample;
            finalCount = count;
          }
          published = true;
        }
      }
      if (mode == 1 || mode == 2)
        require(!published && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                         : TaskFailure::DeadlineExceeded),
                "Bool cancellation published");
      else if (mode == 4)
        require(!published && failure.kind == TaskFailure::AdmissionRejected,
                "Bool admission accepted");
      else if (mode >= 5)
        require(!published && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid Bool descriptor/byte accepted");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!published && (failure.kind == TaskFailure::AdmissionRejected ||
                               (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation fault changed");
      else
        require(published, "Bool worker failed");
      require(core->close(group) && core->drain() && core->release(group), "group cleanup failed");
      const auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                  !stats.unfinished,
              "scheduler leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases(),
              "Bool owners leaked");
    }
    core.reset();
    if (mode == 0)
      verify(survivor, surviving, finalSample, finalCount);
    survivor.settle();
    require(!survivor.live() && survivor.allocations() == survivor.releases(),
            "Bool survivor leaked");
    std::cout << "Bool array workers " << limits.workers << " mode " << mode
              << ": exact lanes, cached results and cleanup passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
