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
using Raw = std::conditional_t<sizeof(Cell) == 4, std::uint32_t, std::uint64_t>;
template <std::size_t Rank> struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset;
  std::array<std::int64_t, Rank> sizes, strides;
};
struct Reading {
  Cell amount;
  bool enabled;
  Descriptor<1> values;
  Descriptor<2> matrix;
};
struct Result {
  Failure failure;
  Descriptor<1> value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static Raw bits(unsigned sample, unsigned lane) {
  if (sample < 4) {
    constexpr unsigned width = 8 * sizeof(Cell);
    constexpr Raw exponent = sizeof(Cell) == 4 ? Raw(0x7f800000) : Raw(0x7ff0000000000000ULL);
    const std::array<Raw, 8> edges{0,
                                   Raw(Raw(1) << (width - 1)),
                                   1,
                                   Raw(~Raw(0)),
                                   exponent,
                                   Raw(exponent | 1),
                                   Raw(exponent | (Raw(1) << (width - 1))),
                                   Raw(~(Raw(1) << (width - 1)))};
    return edges[lane % edges.size()];
  }
  return Raw(UINT64_C(0x9e3779b97f4a7c15) * (sample * 31 + lane + 1));
}
static void verify(Invocation &context, const Result &result, unsigned sample, std::size_t count) {
  require(result.failure.kind == TaskFailure::None && result.value.sizes[0] == std::int64_t(count),
          "record array metadata changed");
  if (!count) {
    require(!result.value.allocated && !result.value.aligned && !result.value.strides[0],
            "empty record array allocated");
    return;
  }
  auto backing = context.lease(result.value.allocated);
  require(backing && backing->bytes().size() >= count * sizeof(Reading) &&
              result.value.offset == 0 && result.value.strides[0] == 1,
          "record array backing lost");
  for (std::size_t i = 0; i < count; ++i) {
    Reading item;
    std::memcpy(&item, backing->bytes().data() + i * sizeof(Reading), sizeof(item));
    require(std::bit_cast<Raw>(item.amount) == bits(sample, i) && item.enabled == bool(i % 2),
            "record scalar bits changed");
    const std::array<Raw, 2> expected{bits(sample, i + 1), bits(sample, i + 2)};
    for (void *pointer : {item.values.allocated, item.matrix.allocated}) {
      auto payload = context.lease(pointer);
      require(payload && payload->bytes().size() == sizeof(expected) &&
                  !std::memcmp(payload->bytes().data(), expected.data(), sizeof(expected)),
              "nested array/tensor bits changed");
    }
    require(item.values.sizes[0] == 2 && item.values.strides[0] == 1 &&
                item.matrix.sizes == std::array<std::int64_t, 2>{1, 2} &&
                item.matrix.strides == std::array<std::int64_t, 2>{2, 1},
            "nested record descriptor changed");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 4, "expected workers, mode and failure position");
    const auto mode = std::atoi(argv[2]);
    TaskLimits limits;
    // Large successful payload cases include repeated conservative backing
    // reservations across nested tasks and cached observations.
    limits.bytes = 64 * 1024 * 1024;
    limits.workers = unsigned(std::strtoul(argv[1], nullptr, 10));
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "scheduler creation failed");
    if (mode == 3)
      core->failAllocationAt(std::strtoull(argv[3], nullptr, 10));
    auto *entry = kalandra_task_test_entry;
#ifdef KALANDRA_RECORD_ARRAY_FIELD
    // The private wrapper has one descriptor field and the same physical bytes
    // as the root array fixture. Its nominal task layout is a record identity.
    constexpr std::uint64_t expectedIdentity = 0x50001;
#else
    constexpr std::uint64_t expectedIdentity = 0x60000;
#endif
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->result->bytes == sizeof(Result) &&
                entry->result->identity == expectedIdentity,
            "record array task layout mismatch");
    Invocation survivor;
    Result surviving{};
    unsigned finalSample = 0;
    std::size_t finalCount = 0;
    for (unsigned sample = 0; sample < (mode == 0 ? 16u : 1u); ++sample) {
#ifdef KALANDRA_RECORD_ARRAY_LARGE
      const std::size_t count = mode == 0 ? std::array<std::size_t, 4>{0, 1, 257, 1025}[sample % 4] : 7;
#else
      const std::size_t count = mode == 0 ? std::array<std::size_t, 4>{0, 1, 7, 31}[sample % 4] : 7;
#endif
      Invocation source, destination;
      auto *storage = count ? source.allocateRecord(count * sizeof(Reading)) : nullptr;
      require(!count || storage, "source array allocation failed");
      for (std::size_t i = 0; i < count; ++i) {
        auto *data = source.allocateRecord(2 * sizeof(Cell));
        require(data, "nested source allocation failed");
        const std::array<Raw, 2> raw{bits(sample, i + 1), bits(sample, i + 2)};
        std::memcpy(data, raw.data(), sizeof(raw));
        Reading item{std::bit_cast<Cell>(bits(sample, i)),
                     bool(i % 2),
                     {data, data, 0, {2}, {1}},
                     {data, data, 0, {1, 2}, {2, 1}}};
        if (!i && mode == 5)
          item.values.strides[0] = 2;
        if (!i && mode == 7) {
          item.matrix.sizes = {2, 1};
          item.matrix.strides = {1, 1};
        }
        std::memcpy(static_cast<std::byte *>(storage) + i * sizeof(Reading), &item, sizeof(item));
      }
      Result packet{{}, {storage, storage, 0, {std::int64_t(count)}, {count ? 1 : 0}}};
      if (mode == 6)
        packet.value.offset = 1;
      if (mode == 0 && count) {
        // The retention loop must poll cancellation and leave output untouched.
        std::atomic<bool> cancelled = true;
        std::atomic<std::uint64_t> polls = 0;
        Invocation stopped;
        stopped.bindCancellation(&cancelled);
        stopped.watchPolls(&polls);
        alignas(Result) std::array<std::byte, sizeof(Result)> output;
        output.fill(std::byte{0x5a});
        const auto before = output;
        require(!entry->result->copy(stopped, output.data(), source, &packet) &&
                    polls.load() == 1 && output == before,
                "record array retention ignored cancellation or published output");
        stopped.settle();
        require(!stopped.live() && stopped.allocations() == stopped.releases(),
                "cancelled retention leaked");
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
        if (mode == 0 && count)
          for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            Invocation rejected;
            rejected.failAllocationAt(attempt);
            alignas(Result) std::array<std::byte, sizeof(Result)> output;
            output.fill(std::byte{0x5a});
            const auto before = output;
            require(!core->observe(task, *entry->result, rejected, output.data()) &&
                        rejected.status() == 2 && output == before,
                    "failed array observation published partial payload");
            rejected.settle();
            require(!rejected.live() && rejected.allocations() == rejected.releases(),
                    "partial array retain leaked");
          }
        struct Guarded {
          std::uint64_t before = 17;
          Result result{};
          std::uint64_t after = 23;
        } guarded;
        require(core->observe(task, *entry->result, destination, &guarded.result),
                "record array observation failed");
        failure = guarded.result.failure;
        if (failure.kind == TaskFailure::None) {
          verify(destination, guarded.result, sample, count);
          guarded.result = {};
          require(core->observe(task, *entry->result, destination, &guarded.result),
                  "repeated await failed");
          verify(destination, guarded.result, sample, count);
          require(guarded.before == 17 && guarded.after == 23,
                  "array observation damaged canaries");
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving),
                    "surviving observation failed");
            finalSample = sample;
            finalCount = count;
          }
          published = true;
        }
      }
      if (mode == 1 || mode == 2)
        require(!published && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                         : TaskFailure::DeadlineExceeded),
                "cancel/deadline published record array");
      else if (mode == 4)
        require(!published && failure.kind == TaskFailure::AdmissionRejected,
                "array admission accepted");
      else if (mode >= 5)
        require(!published && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid record array published");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!published && (failure.kind == TaskFailure::AdmissionRejected ||
                               (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "array allocation failure changed kind");
      else
        require(published, "record array worker failed");
      require(core->close(group) && core->drain() && core->release(group), "group cleanup failed");
      const auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.records && !stats.groups &&
                  !stats.unfinished,
              "scheduler owners leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases(),
              "array observation owners leaked");
    }
    core.reset();
    if (mode == 0)
      verify(survivor, surviving, finalSample, finalCount);
    survivor.settle();
    require(!survivor.live() && survivor.allocations() == survivor.releases(),
            "surviving array owners leaked");
    std::cout << "record array " << describeNumeric(element)->name << " workers " << limits.workers
              << " mode " << mode << ": exact nested values, cached outcomes and cleanup passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
