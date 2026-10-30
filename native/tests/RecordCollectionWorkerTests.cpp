#include "../runtime/NumericStorage.h"
#include "../runtime/Tasks.h"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<
    sizeof(Cell) == 1, std::uint8_t,
    std::conditional_t<sizeof(Cell) == 2, std::uint16_t,
                       std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>>>;
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset;
  std::array<std::int64_t, 1> sizes, strides;
};
struct Inner {
  Descriptor values;
  bool enabled;
};
struct Record {
  Inner data;
  Descriptor duplicate;
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
int main(int argc, char **argv) {
  try {
    require(argc == 4, "expected workers, mode, fail position");
    TaskLimits limits;
    limits.workers = unsigned(std::strtoul(argv[1], nullptr, 10));
    const auto mode = std::atoi(argv[2]);
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "scheduler creation failed");
    if (mode == 3)
      core->failAllocationAt(std::strtoull(argv[3], nullptr, 10));
    auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) && entry->result->bytes == sizeof(Result),
            "record descriptor ABI mismatch");
    Invocation survivor;
    Result surviving{};
    std::array<Raw, 8> finalBits{};
    for (unsigned sample = 0; sample < (mode == 0 ? 32u : 1u); ++sample) {
      std::array<Raw, 8> bits{};
      for (unsigned i = 0; i < bits.size(); ++i)
        bits[i] = Raw(UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i));
      if (!sample) {
        if constexpr (sizeof(Cell) == 4)
          bits = {0,
                  Raw(0x80000000),
                  1,
                  Raw(0x7f800000),
                  Raw(0xff800000),
                  Raw(0x7fc01234),
                  Raw(0x7f800001),
                  Raw(0xffffffff)};
        else
          bits = {0,
                  Raw(0x8000000000000000ULL),
                  1,
                  Raw(0x7ff0000000000000ULL),
                  Raw(0xfff0000000000000ULL),
                  Raw(0x7ff8123456789abcULL),
                  Raw(0x7ff0000000000001ULL),
                  Raw(0xffffffffffffffffULL)};
      }
      Invocation source, destination;
      auto *storage = source.allocateRecord(sizeof(bits));
      require(storage, "source allocation failed");
      std::memcpy(storage, bits.data(), sizeof(bits));
      Descriptor descriptor{storage, storage, 0, {8}, {1}};
      Result packet{{}, {{descriptor, true}, descriptor}};
      if (mode == 5)
        packet.value.duplicate.strides[0] = -1;
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
        struct Guarded {
          std::uint64_t before = 17;
          Result output{};
          std::uint64_t after = 23;
        } guarded;
        if (mode == 0)
          for (unsigned attempt = 1; attempt <= 2; ++attempt) {
            Invocation failed;
            failed.failAllocationAt(attempt);
            alignas(Result) std::array<std::byte, sizeof(Result)> output;
            output.fill(std::byte{0x5a});
            const auto original = output;
            require(!core->observe(task, *entry->result, failed, output.data()) &&
                        failed.status() == 2 && output == original,
                    "failed observation published partial record");
            failed.settle();
            require(!failed.live() && failed.allocations() == failed.releases(),
                    "failed observation leaked partial owners");
          }
        require(core->observe(task, *entry->result, destination, &guarded.output),
                "observation failed");
        failure = guarded.output.failure;
        if (failure.kind == TaskFailure::None) {
          for (unsigned repeat = 0; repeat < 2; ++repeat) {
            require(guarded.before == 17 && guarded.after == 23 &&
                        guarded.output.value.data.enabled,
                    "record metadata changed");
            for (auto value : {guarded.output.value.data.values, guarded.output.value.duplicate}) {
              auto lease = destination.lease(value.allocated);
              require(lease && value.offset == 0 && value.sizes[0] == 8 && value.strides[0] == 1 &&
                          !std::memcmp(lease->bytes().data(), bits.data(), sizeof(bits)),
                      "raw numeric payload changed");
            }
            require(core->observe(task, *entry->result, destination, &guarded.output),
                    "repeated await failed");
          }
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving),
                    "surviving observation failed");
            finalBits = bits;
          }
          published = true;
        }
      }
      if (mode == 1 || mode == 2)
        require(!published && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                         : TaskFailure::DeadlineExceeded),
                "cancel/deadline published data");
      else if (mode == 5)
        require(!published && failure.kind == TaskFailure::Fault && failure.code == 3,
                "malformed nested descriptor published");
      else if (mode == 4)
        require(!published && failure.kind == TaskFailure::AdmissionRejected, "admission accepted");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!published && (failure.kind == TaskFailure::AdmissionRejected ||
                               (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation failure changed");
      else
        require(published, "worker did not publish");
      require(core->close(group) && core->drain() && core->release(group), "group cleanup failed");
      auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                  !stats.unfinished,
              "scheduler owners leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases(),
              "observed owners leaked");
    }
    core.reset();
    if (mode == 0) {
      auto lease = survivor.lease(surviving.value.data.values.allocated);
      require(lease && !std::memcmp(lease->bytes().data(), finalBits.data(), sizeof(finalBits)),
              "record storage lost after scheduler destruction");
    }
    survivor.settle();
    require(survivor.allocations() == survivor.releases(), "survivor owners leaked");
    std::cout << "collection record worker raw-bit/duplicate/cached/lifetime checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
