#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_worker_test_entry;
extern const ValueLayout *const kalandra_worker_test_layout;
extern void *const kalandra_worker_good_target;
extern void *const kalandra_worker_wrong_target;
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Scalar {
  Header header;
  std::int64_t value = 0;
};
struct Callable {
  Header header;
  void *code = nullptr;
  void *environment = nullptr;
};
struct Arguments {
  Scalar value;
  Callable function;
};
static_assert(sizeof(Header) == 32 && sizeof(Scalar) == 40 && sizeof(Callable) == 48);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void balanced(Invocation &context) {
  context.settle();
  require(context.live() == 0 && context.allocations() == context.releases() &&
              context.callDepth() == 0,
          "worker callable context leaked storage or call admission");
}
int main() {
  try {
    const auto &layout = *kalandra_worker_test_layout;
    const auto &entry = *kalandra_worker_test_entry;
    require(layout.bytes == sizeof(Callable) && layout.alignment == alignof(Callable) &&
                entry.packetBytes == sizeof(Arguments),
            "private callable packet layout changed");
    Callable valid;
    valid.code = kalandra_worker_good_target;
    Invocation source;
    for (unsigned mode = 0; mode < 8; ++mode) {
      Invocation destination;
      std::atomic<bool> cancelled{true};
      auto input = valid;
      if (mode == 1)
        input.code = nullptr;
      if (mode == 2)
        input.code = kalandra_worker_wrong_target;
      if (mode == 3)
        input.environment = reinterpret_cast<void *>(1);
      if (mode == 4)
        input.header.kind = 1;
      if (mode == 7)
        input.code = reinterpret_cast<void *>(1);
      if (mode == 5)
        destination.fault(4, 9, 7);
      if (mode == 6)
        destination.bindCancellation(&cancelled);
      std::array<std::byte, sizeof(Callable)> output;
      output.fill(std::byte{0x5a});
      const auto before = output;
      // The trusted ValueLayout requires aligned destination storage.
      alignas(Callable) std::array<std::byte, sizeof(Callable)> aligned = output;
      const bool ok = layout.copy(destination, aligned.data(), source, &input);
      if (mode == 0) {
        Callable copied;
        std::memcpy(&copied, aligned.data(), sizeof copied);
        require(ok && copied.code == valid.code && !copied.environment && !destination.status(),
                "valid capture-free function copy failed");
      } else {
        require(!ok && aligned == before, "invalid/stopped function copy published output");
        require(destination.status() == (mode == 5   ? 4
                                         : mode == 6 ? 0
                                                     : 3),
                "function copy replaced a stopped context or lost metadata fault");
        if (mode == 5)
          require(destination.line() == 9 && destination.column() == 7,
                  "function copy replaced primary fault location");
      }
      require(destination.allocations() == 0, "capture-free handoff allocated an environment");
      balanced(destination);
    }
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "worker test core creation failed");
      auto group = core->openRoot();
      auto parent = std::make_unique<Invocation>();
      Arguments arguments;
      arguments.value.value = 21;
      arguments.function = valid;
      std::vector<TaskHandle> handles;
      for (unsigned n = 0; n < 16; ++n) {
        auto handle = core->submit(
            group, entry, {entry.packetLayout, &arguments, sizeof arguments, parent.get()});
        require(bool(handle), "named callable task not admitted");
        handles.push_back(handle);
      }
      balanced(*parent);
      parent.reset(); // No child may depend on this invocation's lifetime.
      require(core->close(group) && core->drain(), "named callable group did not drain");
      Invocation observer;
      for (auto handle : handles) {
        require(core->settled(handle) && core->failure(handle).kind == TaskFailure::None,
                "named callable worker failed");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          Scalar output;
          require(core->observe(handle, *entry.result, observer, &output) && !output.header.kind &&
                      output.value == 42,
                  "named callable cached outcome changed");
        }
      }
      require(core->release(group), "named callable group release failed");
      balanced(observer);
      const auto stats = core->stats();
      require(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedBytes &&
                  !stats.retainedOwners,
              "named callable scheduler leaked owners/reservations");
    }
    balanced(source);
    std::cout << "Worker callable metadata: 8 copy cases; 48 tasks and 96 observations across "
                 "1/2/4 workers passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
