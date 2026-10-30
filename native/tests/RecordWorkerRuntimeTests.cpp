#include "../runtime/NumericStorage.h"
#include "../runtime/Tasks.h"
#include <array>
#include <bit>
#include <cstdlib>
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
#ifdef NESTED_RECORD
struct Position {
  Cell x;
  bool valid;
  Cell y;
};
struct Metadata {
  std::uint64_t stamp;
  bool ready;
};
struct Reading {
  bool enabled;
  Position position;
  Metadata metadata;
};
struct Record {
  Reading reading;
  std::int32_t generation;
};
static Record makeRecord(Raw x, Raw y, bool enabled) {
  return {{enabled,
           {std::bit_cast<Cell>(x), !enabled, std::bit_cast<Cell>(y)},
           {UINT64_C(0xfedcba9876543210), true}},
          -17};
}
static bool same(const Record &record, Raw x, Raw y, bool enabled) {
  const auto &r = record.reading;
  return std::bit_cast<Raw>(r.position.x) == x && std::bit_cast<Raw>(r.position.y) == y &&
         r.enabled == enabled && r.position.valid == !enabled &&
         r.metadata.stamp == UINT64_C(0xfedcba9876543210) && r.metadata.ready &&
         record.generation == -17;
}
constexpr const char *recordLabel = "nested record worker ";
#else
struct Record {
  Cell x, y;
  bool enabled;
};
static Record makeRecord(Raw x, Raw y, bool enabled) {
  return {std::bit_cast<Cell>(x), std::bit_cast<Cell>(y), enabled};
}
static bool same(const Record &record, Raw x, Raw y, bool enabled) {
  return std::bit_cast<Raw>(record.x) == x && std::bit_cast<Raw>(record.y) == y &&
         record.enabled == enabled;
}
constexpr const char *recordLabel = "record worker ";
#endif
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
    require(argc == 4, "expected worker count, mode, failure position");
    const auto workers = unsigned(std::strtoul(argv[1], nullptr, 10));
    const auto mode = std::atoi(argv[2]);
    const auto attempt = std::strtoull(argv[3], nullptr, 10);
    TaskLimits limits;
    limits.workers = workers;
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "cannot create scheduler");
    if (mode == 3)
      core->failAllocationAt(attempt);
    const auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->packetAlignment == alignof(Result) && entry->result &&
                entry->result->bytes == sizeof(Result) &&
                entry->result->alignment == alignof(Result) && entry->result->identity == 0x50000,
            "record packet/result layout mismatch");
    const std::array<Raw, 10> edges = sizeof(Cell) == 4
                                          ? std::array<Raw, 10>{0,
                                                                Raw(0x80000000),
                                                                1,
                                                                Raw(0x7f800000),
                                                                Raw(0xff800000),
                                                                Raw(0x7fc01234),
                                                                Raw(0x7f800001),
                                                                Raw(0xffffffff),
                                                                Raw(0x7fffffff),
                                                                Raw(0x7f7fffff)}
                                          : std::array<Raw, 10>{0,
                                                                Raw(0x8000000000000000ULL),
                                                                1,
                                                                Raw(0x7ff0000000000000ULL),
                                                                Raw(0xfff0000000000000ULL),
                                                                Raw(0x7ff8123456789abcULL),
                                                                Raw(0x7ff0000000000001ULL),
                                                                Raw(0xffffffffffffffffULL),
                                                                Raw(0x7fffffffffffffffULL),
                                                                Raw(0x7fefffffffffffffULL)};
    std::uint64_t seed = 0x68742abc9123def0ULL;
    Result surviving{};
    Raw finalX = 0, finalY = 0;
    bool finalEnabled = false, anySuccess = false;
    const unsigned count = mode == 0 ? 64 : 1;
    for (unsigned sample = 0; sample < count; ++sample) {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      const auto x = sample < edges.size() ? edges[sample] : Raw(seed);
      const auto y = sample < edges.size() ? edges[edges.size() - 1 - sample] : Raw(~seed);
      const bool enabled = sample % 2;
      Result packet{{}, makeRecord(x, y, enabled)};
      auto group = core->openRoot();
      require(bool(group), "root group rejected");
      Invocation source;
      auto task =
          core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
      // Submission snapshots scalar fields; the caller's storage is no dependency.
      packet = {};
      source.settle();
      if (mode == 1 || mode == 2)
        require(core->cancel(group, mode == 2), "cancellation rejected");
      require(core->drain(), "scheduler did not settle");
      auto failure = core->failure(group);
      if (failure.kind == TaskFailure::None && task)
        failure = core->failure(task);
      Invocation destination;
      struct Guarded {
        std::uint64_t before = 0x123456789abcdef0ULL;
        Result output{};
        std::uint64_t after = 0xfedcba9876543210ULL;
      } guarded;
      bool published = false;
      if (failure.kind == TaskFailure::None && task) {
        require(core->observe(task, *entry->result, destination, &guarded.output),
                "record observation failed");
        failure = guarded.output.failure;
        if (failure.kind == TaskFailure::None) {
          published = true;
          require(same(guarded.output.value, x, y, enabled),
                  "record bits changed in worker transport");
          require(guarded.before == 0x123456789abcdef0ULL && guarded.after == 0xfedcba9876543210ULL,
                  "record observation overwrote guards");
          // An observed copy cannot mutate the cached result.
          guarded.output.value = {};
          require(core->observe(task, *entry->result, destination, &guarded.output) &&
                      guarded.output.failure.kind == TaskFailure::None &&
                      same(guarded.output.value, x, y, enabled),
                  "repeated record await changed cached value");
          auto wrong = *entry->result;
          ++wrong.identity; // Same-sized sibling nominal record.
          Invocation invalid;
          alignas(Result) std::array<std::byte, sizeof(Result)> bytes;
          bytes.fill(std::byte{0x5a});
          const auto before = bytes;
          require(!core->observe(task, wrong, invalid, bytes.data()) && invalid.status() == 3 &&
                      bytes == before && !invalid.live(),
                  "wrong nominal record layout published output");
          surviving = guarded.output;
          finalX = x;
          finalY = y;
          finalEnabled = enabled;
          anySuccess = true;
        }
      }
      if (mode == 1 || mode == 2)
        require(!published && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                         : TaskFailure::DeadlineExceeded),
                "cancelled record was published");
      else if (mode == 4)
        require(!published && failure.kind == TaskFailure::AdmissionRejected,
                "record admission failure lost");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!published && ((failure.kind == TaskFailure::Fault && failure.code == 2) ||
                               failure.kind == TaskFailure::AdmissionRejected),
                "record allocation failure changed kind or published output");
      else
        require(published && failure.kind == TaskFailure::None, "record worker failed");
      require(core->close(group) && core->drain() && core->release(group),
              "cannot release record group");
      const auto stats = core->stats();
      require(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished &&
                  !stats.records && !stats.groups && !stats.ready && !stats.pending &&
                  !stats.maintenanceTasks && !stats.externalPending,
              "record scheduler resources leaked");
      destination.settle();
      require(!source.live() && !destination.live() && source.allocations() == source.releases() &&
                  destination.allocations() == destination.releases(),
              "record transport leaked owners");
      require(!destination.allocations(),
              "scalar-field record observation unexpectedly allocated payload storage");
    }
    core.reset();
    if (anySuccess)
      require(same(surviving.value, finalX, finalY, finalEnabled),
              "record did not survive scheduler destruction");
    std::cout << recordLabel << describeNumeric(element)->name << " workers " << workers << " mode "
              << mode << " allocation " << attempt << ": " << count
              << " exact snapshot/cached/layout/settlement cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
