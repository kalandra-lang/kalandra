#include "../runtime/String.h"
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
  StringDescriptor value;
};
extern "C" const TaskEntry *const kalandra_task_test_entry;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static bool same(Invocation &ctx, const StringDescriptor &value, const std::string &expected) {
  return validateString(ctx, value) && value.byteLength == std::int64_t(expected.size()) &&
         (!value.byteLength || !std::memcmp(value.data, expected.data(), expected.size()));
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected worker count");
    const auto workers = unsigned(std::stoul(argv[1]));
    const auto *entry = kalandra_task_test_entry;
    require(entry && entry->packetBytes == sizeof(Result) && entry->result &&
                entry->result->bytes == sizeof(Result) &&
                entry->result->alignment == alignof(Result),
            "String packet layout mismatch");
    // Retention shares immutable bytes; each destination owns its own reference.
    for (unsigned mode = 0; mode < 7; ++mode) {
      Invocation source, destination;
      StringDescriptor value;
      kalandra_string_literal_v1(&source, &value, "é", 2);
      std::atomic<bool> stopped = true;
      if (mode == 1)
        source.settle();
      if (mode == 2)
        source.fault(4);
      if (mode == 3)
        destination.settle();
      if (mode == 4)
        destination.fault(4);
      if (mode == 5)
        destination.failAllocationAt(1);
      if (mode == 6)
        destination.bindCancellation(&stopped);
      const bool retained = kalandra_string_retain_v1(&destination, &source, &value);
      if (mode == 0) {
        require(retained && destination.live() == 1, "valid retain failed");
        source.settle();
        require(same(destination, value, "é"), "String backing lost after source settlement");
      } else
        require(!retained && !destination.live(), "invalid/cancelled owner retained");
    }
    {
      Invocation source, destination;
      StringDescriptor empty;
      require(kalandra_string_retain_v1(&destination, &source, &empty) &&
                  !destination.allocations(),
              "empty retain allocated storage");
      StringDescriptor large;
      const std::string text(16 * 1024 * 1024, 'a');
      kalandra_string_literal_v1(&source, &large, text.data(), text.size());
      std::atomic<bool> stop = false;
      std::atomic<std::uint64_t> polls = 0;
      destination.bindCancellation(&stop);
      destination.watchPolls(&polls);
      std::thread cancel([&] {
        while (polls.load(std::memory_order_acquire) < 3)
          std::this_thread::yield();
        stop.store(true, std::memory_order_release);
      });
      const bool retained = kalandra_string_retain_v1(&destination, &source, &large);
      cancel.join();
      require(!retained && !destination.live() && !destination.allocations(),
              "cancelled validation retained a String");
    }
    const std::array<std::string, 5> texts = {"", "a", std::string("é\0👩‍💻", 14), "سلام",
                                              std::string(8192, 'a') + "é"};
    for (unsigned mode = 0; mode < 5; ++mode)
      for (const auto &text : texts) {
        TaskLimits limits;
        limits.workers = workers;
        if (mode == 3)
          limits.bytes = 1;
        auto core = TaskCore::create(limits);
        require(bool(core), "scheduler creation failed");
        if (mode == 4)
          core->failAllocationAt(1);
        auto group = core->openRoot();
        Invocation source, destination;
        Result packet{};
        kalandra_string_literal_v1(&source, &packet.value, text.data(), text.size());
        require(!source.status(), "String input construction failed");
        auto handle =
            core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
        source.settle(); // Worker must retain the backing before submit returns.
        packet = {};
        if (mode == 1 || mode == 2)
          require(core->cancel(group, mode == 2), "cancellation failed");
        require(core->drain(), "worker drain failed");
        auto failure = core->failure(group);
        if (failure.kind == TaskFailure::None && handle)
          failure = core->failure(handle);
        Result output{};
        bool success = false;
        if (handle && failure.kind == TaskFailure::None) {
          require(core->observe(handle, *entry->result, destination, &output), "observe failed");
          failure = output.failure;
          success = failure.kind == TaskFailure::None;
        }
        if (mode == 0) {
          require(success && same(destination, output.value, text), "worker String bytes changed");
          output = {};
          require(core->observe(handle, *entry->result, destination, &output) &&
                      same(destination, output.value, text),
                  "cached String await changed value");
          auto wrong = *entry->result;
          ++wrong.identity;
          Invocation invalid;
          alignas(Result) std::array<std::byte, sizeof(Result)> guarded;
          guarded.fill(std::byte{0x5a});
          const auto before = guarded;
          require(!core->observe(handle, wrong, invalid, guarded.data()) && guarded == before &&
                      !invalid.live(),
                  "mismatched String layout published output");
          // A failed owner allocation must not publish a descriptor.
          if (!text.empty()) {
            Invocation failing;
            failing.failAllocationAt(1);
            require(!core->observe(handle, *entry->result, failing, guarded.data()) &&
                        failing.status() == 2 && guarded == before && !failing.live(),
                    "failed String retain published output");
          }
        } else {
          require(!success, "failed/cancelled String task published output");
          require((mode == 4 && failure.kind == TaskFailure::Fault && failure.code == 2) ||
                      failure.kind == (mode == 1   ? TaskFailure::Cancelled
                                       : mode == 2 ? TaskFailure::DeadlineExceeded
                                                   : TaskFailure::AdmissionRejected),
                  ("String task failure variant changed: mode " + std::to_string(mode) + " kind " +
                   std::to_string(int(failure.kind)) + " code " + std::to_string(failure.code))
                      .c_str());
        }
        core->close(group);
        require(core->drain(), "closed group drain failed");
        require(core->release(group), "group release failed");
        const auto stats = core->stats();
        require(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished &&
                    !stats.records && !stats.groups && !stats.ready && !stats.pending &&
                    !stats.maintenanceTasks && !stats.externalPending,
                "String scheduler resources leaked");
        core.reset();
        if (success)
          require(same(destination, output.value, text), "String lost after scheduler destruction");
        destination.settle();
        require(source.allocations() == source.releases() &&
                    destination.allocations() == destination.releases() && !destination.live(),
                "String task owners leaked");
      }
    // Forged descriptors cannot become retained task inputs.
    for (unsigned mode = 0; mode < 5; ++mode) {
      auto core = TaskCore::create();
      auto group = core->openRoot();
      Invocation source;
      Result packet{};
      kalandra_string_literal_v1(&source, &packet.value, "abc", 3);
      if (mode == 0)
        packet.value.data++;
      if (mode == 1)
        packet.value.byteLength = 4;
      if (mode == 2)
        packet.value.byteLength = -1;
      if (mode == 3)
        packet.value.allocated = reinterpret_cast<void *>(1);
      if (mode == 4)
        const_cast<char *>(packet.value.data)[2] = char(0x80);
      auto handle =
          core->submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet), &source});
      require(core->drain(), "invalid String drain failed");
      auto failure = handle ? core->failure(handle) : core->failure(group);
      require(failure.kind != TaskFailure::None, "forged String admitted");
      core->close(group);
      require(core->drain(), "invalid closed group drain failed");
      require(core->release(group), "invalid String group leaked");
    }
    std::cout << "String workers " << workers
              << ": owned inputs/results, cached awaits, "
                 "malformed packets, cancellation and failure settlement passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
