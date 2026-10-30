#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
using namespace kalandra::runtime;
struct Descriptor {
  void *allocated, *aligned;
  std::int64_t offset, size, stride;
};
struct Inner {
  StringArrayDescriptor names;
  bool enabled;
  StringDescriptor label;
};
struct Record {
  Inner data;
  StringArrayDescriptor duplicate;
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
static const std::array<std::string, 3> texts = {"", "é", std::string("a\0👩‍💻", 13)};
static StringDescriptor cell(const StringArrayDescriptor &array, unsigned i) {
  StringDescriptor out;
  std::memcpy(&out, static_cast<const std::byte *>(array.data) + i * sizeof(out), sizeof(out));
  return out;
}
static void verify(Invocation &ctx, const Result &result, unsigned count, unsigned sample) {
  require(result.failure.kind == TaskFailure::None && result.value.data.enabled,
          "record metadata changed");
  const auto &record = result.value;
  require(record.data.names.allocated == record.duplicate.allocated, "shared array backing copied");
  for (const auto &array : {record.data.names, record.duplicate}) {
    kalandra_string_array_validate_v1(&ctx, &array);
    require(!ctx.status() && array.size == count, "String array field invalid");
    for (unsigned i = 0; i < count; ++i) {
      const auto value = cell(array, i);
      require(value.byteLength == std::int64_t(texts[i % 3].size()) &&
                  (!value.byteLength ||
                   !std::memcmp(value.data, texts[i % 3].data(), value.byteLength)),
              "field text changed");
      if (i >= 3)
        require(value.allocated == cell(array, i - 3).allocated, "shared text backing copied");
    }
  }
  const auto &label = record.data.label;
  require(validateString(ctx, label) && label.byteLength == 2 && !std::memcmp(label.data, "é", 2),
          "scalar String sibling lost");
  if (count > 1)
    require(label.allocated == cell(record.data.names, 1).allocated,
            "scalar/array shared text copied");
  const auto &numbers = record.numbers;
  auto lease = ctx.lease(numbers.allocated);
  require(lease && numbers.size == 8 && numbers.stride == 1 && !numbers.offset &&
              numbers.aligned == numbers.allocated && lease->bytes().size() == 64,
          "numeric sibling invalid");
  for (unsigned i = 0; i < 8; ++i) {
    std::uint64_t value;
    std::memcpy(&value, lease->bytes().data() + i * 8, 8);
    require(value == UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i),
            "numeric sibling bits changed");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 4, "expected workers, mode, allocation position");
    const auto mode = std::atoi(argv[2]);
    TaskLimits limits;
    limits.workers = std::stoul(argv[1]);
    if (mode == 4)
      limits.bytes = 1;
    auto core = TaskCore::create(limits);
    require(bool(core), "scheduler creation failed");
    if (mode == 3)
      core->failAllocationAt(std::strtoull(argv[3], nullptr, 10));
    const auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->result->bytes == sizeof(Result) && entry->result->identity == 0x50000,
            "String-array field layout mismatch");
    Invocation survivor;
    Result surviving{};
    unsigned lastCount = 0, lastSample = 0;
    for (unsigned sample = 0; sample < (mode == 0 ? 12u : 1u); ++sample) {
      const unsigned count = mode == 0 ? std::array<unsigned, 4>{0, 1, 7, 257}[sample % 4] : 7;
      Invocation source, destination;
      std::array<StringDescriptor, 3> values{};
      for (unsigned i = 0; i < 3; ++i)
        kalandra_string_literal_v1(&source, &values[i], texts[i].data(), texts[i].size());
      auto *storage =
          count ? static_cast<std::byte *>(source.allocateRecord(count * sizeof(StringDescriptor)))
                : nullptr;
      for (unsigned i = 0; i < count; ++i)
        std::memcpy(storage + i * sizeof(StringDescriptor), &values[i % 3],
                    sizeof(StringDescriptor));
      auto *numbers = static_cast<std::byte *>(source.allocateRecord(64));
      require(numbers && !source.status(), "source allocation failed");
      for (unsigned i = 0; i < 8; ++i) {
        auto raw = UINT64_C(0x9e3779b97f4a7c15) * (sample * 8 + i);
        std::memcpy(numbers + i * 8, &raw, 8);
      }
      StringArrayDescriptor names{storage, storage, 0, count, count ? 1 : 0};
      Result packet{{}, {{names, true, values[1]}, names, {numbers, numbers, 0, 8, 1}}};
      if (mode == 5) {
        auto bad = cell(names, count - 2);
        bad.data = reinterpret_cast<const char *>(1);
        std::memcpy(storage + (count - 2) * sizeof(bad), &bad, sizeof(bad));
      }
      if (mode == 6)
        packet.value.duplicate.stride = -1;
      if (mode == 7)
        packet.value.numbers.size = 9;
      if (mode == 8)
        packet.value.data.names.offset = 1;
      if (mode == 9)
        packet.value.data.label.byteLength = 3;
      alignas(Result) std::array<std::byte, sizeof(Result)> out;
      out.fill(std::byte{0x5a});
      const auto before = out;
      if (mode >= 5) {
        Invocation invalid;
        require(!entry->result->copy(invalid, out.data(), source, &packet) &&
                    invalid.status() == 3 && out == before,
                "invalid field published output");
        invalid.settle();
        require(!invalid.live() && invalid.allocations() == invalid.releases(),
                "partial field validation leaked");
      }
      if (mode == 0) {
        unsigned nonempty = 0;
        for (unsigned i = 0; i < count; ++i)
          nonempty += i % 3 != 0;
        const unsigned owners = 2 * (count ? 1 : 0) + 2 * nonempty + 2;
        std::set<unsigned> attempts{1, 2, owners - 1, owners};
        for (unsigned i = 3; i <= 6 && i <= owners; ++i)
          attempts.insert(i);
        for (auto fail : attempts) {
          Invocation rejected;
          rejected.failAllocationAt(fail);
          require(!entry->result->copy(rejected, out.data(), source, &packet) &&
                      rejected.status() == 2 && out == before,
                  "failed field retain published");
          rejected.settle();
          require(!rejected.live() && rejected.allocations() == rejected.releases(),
                  "partial retention leaked");
        }
        Invocation stopped;
        std::atomic<bool> stop = true;
        stopped.bindCancellation(&stop);
        require(!entry->result->copy(stopped, out.data(), source, &packet) && out == before &&
                    !stopped.live(),
                "cancelled field copy published");
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
          verify(destination, result, count, sample);
          result = {};
          require(core->observe(task, *entry->result, destination, &result),
                  "cached observe failed");
          verify(destination, result, count, sample);
          if (mode == 0) {
            require(core->observe(task, *entry->result, survivor, &surviving), "survivor failed");
            lastCount = count;
            lastSample = sample;
          }
        }
      }
      if (mode == 1 || mode == 2)
        require(!success && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                       : TaskFailure::DeadlineExceeded),
                "cancelled field result published");
      else if (mode == 4)
        require(!success && failure.kind == TaskFailure::AdmissionRejected,
                "admission failure lost");
      else if (mode >= 5)
        require(!success && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid field admitted");
      else if (mode == 3 && failure.kind != TaskFailure::None)
        require(!success && (failure.kind == TaskFailure::AdmissionRejected ||
                             (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation failure changed");
      else
        require(success, "String-array record worker failed");
      require(core->close(group) && core->drain() && core->release(group), "group release failed");
      auto stats = core->stats();
      require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups &&
                  !stats.unfinished && !stats.ready && !stats.pending,
              "scheduler leaked");
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases() &&
                  source.allocations() == source.releases(),
              "field owners leaked");
    }
    core.reset();
    if (mode == 0)
      verify(survivor, surviving, lastCount, lastSample);
    survivor.settle();
    require(!survivor.live() && survivor.allocations() == survivor.releases(),
            "surviving fields leaked");
    std::cout << "String-array record workers " << limits.workers << " mode " << mode
              << ": shared arrays/text, nested retention, exact siblings and settlement passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
