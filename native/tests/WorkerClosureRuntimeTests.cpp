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
extern const TaskEntry *const kalandra_closure_test_entry;
extern const ValueLayout *const kalandra_closure_test_layout;
extern void *const kalandra_closure_test_code;
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
  void *code = nullptr, *environment = nullptr;
};
struct Arguments {
  Scalar value;
  Callable function;
};
struct View {
  void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0, count = 2, stride = 1;
};
static_assert(sizeof(View) == 40 && sizeof(Callable) == 48 && sizeof(Scalar) == 40);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static void balanced(Invocation &context) {
  context.settle();
  require(!context.live() && !context.environmentBytes() && !context.callDepth() &&
              context.allocations() == context.releases(),
          "closure context leaked storage/reservations/admission");
}
static Callable make(Invocation &source) {
  auto *values = static_cast<std::int64_t *>(source.allocateRecord(16));
  require(values, "capture allocation failed");
  values[0] = 20;
  values[1] = 20;
  View view;
  view.allocated = view.aligned = values;
  auto *environment = source.allocateEnvironment(sizeof(View), kalandra_closure_test_code);
  require(environment, "environment allocation failed");
  std::memcpy(environment, &view, sizeof view);
  Callable function;
  function.code = kalandra_closure_test_code;
  function.environment = environment;
  return function;
}
using Invoke = std::int64_t (*)(void *, std::int64_t, Invocation *);
int main() {
  try {
    const auto &layout = *kalandra_closure_test_layout;
    const auto &entry = *kalandra_closure_test_entry;
    require(layout.bytes == sizeof(Callable) && entry.packetBytes == sizeof(Arguments),
            "private closure ABI changed");
    auto parent = std::make_unique<Invocation>();
    auto original = make(*parent);
    Invocation first, second;
    Callable a, b;
    require(layout.copy(first, &a, *parent, &original) &&
                layout.copy(second, &b, *parent, &original),
            "closure copy failed");
    require(a.environment != original.environment && b.environment != original.environment &&
                a.environment != b.environment,
            "closure environments were not independently copied");
    require(first.environmentBytes() == 40 && second.environmentBytes() == 40,
            "environment bytes not reserved");
    balanced(*parent);
    parent.reset();
    const auto invoke = reinterpret_cast<Invoke>(kalandra_closure_test_code);
    for (unsigned n = 0; n < 256; ++n)
      require(invoke(a.environment, 2, &first) == 42 && !first.status() && !first.callDepth(),
              "copied closure lost capture lifetime/context");
    balanced(first);
    require(invoke(b.environment, 2, &second) == 42 && !second.status(),
            "settling another copy invalidated backing");
    balanced(second);

    Invocation source;
    auto valid = make(source);
    for (unsigned mode = 0; mode < 9; ++mode) {
      Invocation destination;
      std::atomic<bool> cancelled{true};
      auto input = valid;
      if (mode == 0)
        input.environment = reinterpret_cast<void *>(1);
      if (mode == 1) {
        input.environment = source.allocateRecord(40);
        std::memcpy(input.environment, valid.environment, 40);
      }
      if (mode == 2) {
        input.environment = source.allocateEnvironment(40, reinterpret_cast<void *>(1));
        std::memcpy(input.environment, valid.environment, 40);
      }
      if (mode == 3)
        input.environment = source.allocateEnvironment(32, kalandra_closure_test_code);
      if (mode == 4)
        input.environment = static_cast<std::byte *>(valid.environment) + 1;
      if (mode == 5 || mode == 6) {
        auto view = *static_cast<View *>(valid.environment);
        if (mode == 5)
          view.aligned = reinterpret_cast<void *>(1);
        else
          view.count = INT64_MAX;
        input.environment = source.allocateEnvironment(40, kalandra_closure_test_code);
        std::memcpy(input.environment, &view, 40);
      }
      if (mode == 7)
        destination.fault(4, 8, 9);
      if (mode == 8)
        destination.bindCancellation(&cancelled);
      alignas(Callable) std::array<std::byte, sizeof(Callable)> output;
      output.fill(std::byte{0x5a});
      const auto before = output;
      require(!layout.copy(destination, output.data(), source, &input) && output == before,
              "bad closure copy published output");
      require(destination.status() == (mode == 7   ? 4
                                       : mode == 8 ? 0
                                                   : 3),
              "closure copy changed primary failure/cancellation");
      balanced(destination);
    }
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      Invocation destination;
      destination.failAllocationAt(attempt);
      Callable output;
      const auto ok = layout.copy(destination, &output, source, &valid);
      require((attempt == 1 || attempt == 2) ? (!ok && destination.status() == 2)
                                             : (ok && !destination.status()),
              "environment/retention allocation failure lost checked status");
      balanced(destination);
    }
    Invocation limited;
    auto *large = source.allocateEnvironment(524289, kalandra_closure_test_code);
    require(large, "large test environment allocation failed");
    require(limited.cloneEnvironment(source, large, 524289, 1, kalandra_closure_test_code),
            "first bounded environment copy failed");
    require(!limited.cloneEnvironment(source, large, 524289, 1, kalandra_closure_test_code) &&
                limited.status() == 2,
            "invocation environment budget was not enforced");
    balanced(limited);

    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "core allocation failed");
      auto group = core->openRoot();
      auto producer = std::make_unique<Invocation>();
      Arguments arguments;
      arguments.value.value = 2;
      arguments.function = make(*producer);
      std::vector<TaskHandle> handles;
      for (unsigned n = 0; n < 16; ++n) {
        auto task = core->submit(
            group, entry, {entry.packetLayout, &arguments, sizeof arguments, producer.get()});
        require(bool(task), "captured task not admitted");
        handles.push_back(task);
      }
      require(core->stats().environmentBytes == 16 * 40,
              "scheduler did not charge environment copies");
      balanced(*producer);
      producer.reset();
      require(core->close(group) && core->drain(), "captured tasks did not drain");
      Invocation observer;
      for (auto task : handles) {
        require(core->failure(task).kind == TaskFailure::None,
                "captured task failed after producer destruction");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          Scalar output;
          require(core->observe(task, *entry.result, observer, &output) && output.value == 42 &&
                      !output.header.kind,
                  "captured cached outcome changed");
        }
      }
      require(core->release(group), "captured group release failed");
      balanced(observer);
      auto stats = core->stats();
      require(!stats.environmentBytes && !stats.retainedBytes && !stats.retainedOwners &&
                  !stats.records && !stats.groups,
              "captured scheduler cleanup leaked");

      TaskLimits tiny;
      tiny.workers = workers;
      tiny.bytes = entry.frameBytes + entry.result->bytes + 39;
      auto quota = TaskCore::create(tiny);
      require(bool(quota), "quota core creation failed");
      auto qgroup = quota->openRoot();
      Arguments packet;
      packet.value.value = 2;
      packet.function = valid;
      auto denied =
          quota->submit(qgroup, entry, {entry.packetLayout, &packet, sizeof packet, &source});
      require(bool(denied) && quota->close(qgroup) && quota->drain(),
              "quota rejection failed to settle");
      require(quota->failure(denied).kind == TaskFailure::Fault && quota->failure(denied).code == 2,
              "environment quota did not fail checked allocation");
      require(quota->release(qgroup) && !quota->stats().retainedBytes &&
                  !quota->stats().environmentBytes,
              "quota rejection leaked reservations");

      auto cancel = TaskCore::create(limits);
      auto cgroup = cancel->openRoot();
      auto stopped =
          cancel->submit(cgroup, entry, {entry.packetLayout, &packet, sizeof packet, &source});
      require(bool(stopped) && cancel->cancel(cgroup) && cancel->close(cgroup) && cancel->drain(),
              "captured cancellation did not settle");
      require(cancel->failure(stopped).kind == TaskFailure::Cancelled,
              "queued captured task ignored cancellation");
      require(cancel->release(cgroup) && !cancel->stats().retainedBytes &&
                  !cancel->stats().environmentBytes,
              "cancelled closure leaked reservations");
    }
    balanced(source);
    std::cout << "Worker closures: 257 post-creator calls; 9 invalid/stopped copies, 4 allocation "
                 "probes; 48 tasks/96 observations; 3 quota and 3 cancellation cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
