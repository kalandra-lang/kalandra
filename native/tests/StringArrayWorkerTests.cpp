#include "../runtime/StringArray.h"
#include "../runtime/Tasks.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
using namespace kalandra::runtime;
struct Result {
  Failure failure;
  StringArrayDescriptor value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static StringDescriptor cell(const StringArrayDescriptor &a, unsigned i) {
  StringDescriptor value;
  std::memcpy(&value, static_cast<const std::byte *>(a.data) + i * sizeof(value), sizeof(value));
  return value;
}
static const std::array<std::string, 3> texts = {"", "é", std::string("a\0👩‍💻", 13)};
static StringArrayDescriptor make(Invocation &source, unsigned count) {
  std::array<StringDescriptor, 3> values{};
  for (unsigned i = 0; i < 3; ++i)
    kalandra_string_literal_v1(&source, &values[i], texts[i].data(), texts[i].size());
  auto *data =
      count ? static_cast<std::byte *>(source.allocateRecord(count * sizeof(StringDescriptor)))
            : nullptr;
  for (unsigned i = 0; i < count; ++i)
    std::memcpy(data + i * sizeof(StringDescriptor), &values[i % 3], sizeof(StringDescriptor));
  return {data, data, 0, count, count ? 1 : 0};
}
static void verify(Invocation &ctx, const Result &result, unsigned count) {
  kalandra_string_array_validate_v1(&ctx, &result.value);
  require(!ctx.status() && result.failure.kind == TaskFailure::None && result.value.size == count,
          "array result invalid");
  for (unsigned i = 0; i < count; ++i) {
    const auto v = cell(result.value, i);
    require(v.byteLength == std::int64_t(texts[i % 3].size()) &&
                (!v.byteLength || !std::memcmp(v.data, texts[i % 3].data(), v.byteLength)),
            "array text changed");
    if (i >= 3)
      require(v.allocated == cell(result.value, i - 3).allocated, "shared immutable text copied");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected worker count");
    TaskLimits limits;
    limits.workers = std::stoul(argv[1]);
    const auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) &&
                entry->result->bytes == sizeof(Result) && entry->result->identity == 0x80000,
            "String array ABI mismatch");
    for (unsigned mode = 0; mode < 10; ++mode) {
      if (mode == 4)
        limits.bytes = 1;
      else
        limits.bytes = 1024 * 1024;
      auto core = TaskCore::create(limits);
      require(bool(core), "scheduler failed");
      if (mode == 3)
        core->failAllocationAt(1);
      Invocation source, destination;
      const unsigned count = mode == 0 ? 257 : 7;
      Result packet{{}, make(source, count)};
      if (mode == 5)
        packet.value.stride = -1;
      if (mode == 6)
        packet.value.offset = 1;
      if (mode >= 7) {
        auto bad = cell(packet.value, count - 2);
        if (mode == 7)
          bad.data = reinterpret_cast<const char *>(1);
        if (mode == 8)
          bad.byteLength = -1;
        if (mode == 9)
          bad.byteLength = 0; // A noncanonical empty descriptor with backing.
        std::memcpy(static_cast<std::byte *>(packet.value.data) + (count - 2) * sizeof(bad), &bad,
                    sizeof(bad));
      }
      alignas(Result) std::array<std::byte, sizeof(Result)> out;
      out.fill(std::byte{0x5a});
      const auto before = out;
      if (mode >= 5) {
        Invocation invalid;
        require(!entry->result->copy(invalid, out.data(), source, &packet) && out == before &&
                    invalid.status() == 3,
                "malformed array published");
        invalid.settle();
        require(!invalid.live() && invalid.allocations() == invalid.releases(),
                "partial invalid array leaked");
      }
      if (mode == 0) {
        for (unsigned fail = 1; fail <= 7; ++fail) {
          Invocation rejected;
          rejected.failAllocationAt(fail);
          require(!entry->result->copy(rejected, out.data(), source, &packet) && out == before &&
                      rejected.status() == 2,
                  "failed array retain published");
          rejected.settle();
          require(rejected.allocations() == rejected.releases(), "failed retain leaked");
        }
        Invocation stopped;
        std::atomic<bool> stop = true;
        stopped.bindCancellation(&stop);
        require(!entry->result->copy(stopped, out.data(), source, &packet) && out == before &&
                    !stopped.live(),
                "cancelled retain published");
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
      Result result{};
      bool success = false;
      if (failure.kind == TaskFailure::None && task) {
        require(core->observe(task, *entry->result, destination, &result), "observe failed");
        failure = result.failure;
        success = failure.kind == TaskFailure::None;
        if (success) {
          verify(destination, result, count);
          result = {};
          require(core->observe(task, *entry->result, destination, &result),
                  "cached observe failed");
          verify(destination, result, count);
        }
      }
      if (mode == 0)
        require(success, "array worker failed");
      else if (mode == 1 || mode == 2)
        require(!success && failure.kind == (mode == 1 ? TaskFailure::Cancelled
                                                       : TaskFailure::DeadlineExceeded),
                "cancelled array published");
      else if (mode == 4)
        require(!success && failure.kind == TaskFailure::AdmissionRejected,
                "admission failure lost");
      else if (mode >= 5)
        require(!success && failure.kind == TaskFailure::Fault && failure.code == 3,
                "invalid array accepted");
      else
        require(!success && (failure.kind == TaskFailure::AdmissionRejected ||
                             (failure.kind == TaskFailure::Fault && failure.code == 2)),
                "allocation failure lost");
      require(core->close(group) && core->drain() && core->release(group), "release failed");
      auto stats = core->stats();
      require(!stats.retainedOwners && !stats.retainedBytes && !stats.records && !stats.groups &&
                  !stats.unfinished,
              "scheduler leaked");
      core.reset();
      if (success)
        verify(destination, result, count);
      destination.settle();
      require(!destination.live() && destination.allocations() == destination.releases(),
              "array destination leaked");
    }
    // Empty source/destination and cancellation while validating a long element.
    Invocation source, destination;
    auto empty = make(source, 0);
    require(kalandra_string_array_retain_v1(&destination, &source, &empty) && !destination.live(),
            "empty array allocated");
    std::string large(16 * 1024 * 1024, 'a');
    StringDescriptor text;
    kalandra_string_literal_v1(&source, &text, large.data(), large.size());
    auto *storage = source.allocateRecord(sizeof(text));
    std::memcpy(storage, &text, sizeof(text));
    StringArrayDescriptor array{storage, storage, 0, 1, 1};
    std::atomic<bool> stop = false;
    std::atomic<std::uint64_t> polls = 0;
    destination.bindCancellation(&stop);
    destination.watchPolls(&polls);
    std::thread cancel([&] {
      while (polls.load(std::memory_order_acquire) < 3)
        std::this_thread::yield();
      stop = true;
    });
    const auto retained = kalandra_string_array_retain_v1(&destination, &source, &array);
    cancel.join();
    require(!retained, "cancelled long element retained");
    destination.settle();
    require(!destination.live() && destination.allocations() == destination.releases(),
            "cancelled deep retain leaked");
    std::cout << "String array workers " << limits.workers
              << ": shared text, cached results, malformed arrays, partial failure, cancellation "
                 "and settlement passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
