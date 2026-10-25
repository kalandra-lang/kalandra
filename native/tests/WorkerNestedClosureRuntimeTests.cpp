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
extern const TaskEntry *const kalandra_nested_test_entry;
extern const ValueLayout *const kalandra_nested_test_layout;
extern void *const kalandra_nested_test_base;
extern void *const kalandra_nested_test_outer;
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Scalar {
  Header header;
  std::int64_t value = 0;
};
struct Payload {
  void *code = nullptr, *environment = nullptr;
};
struct Callable {
  Header header;
  Payload payload;
};
struct Arguments {
  Scalar value;
  Callable function;
};
struct View {
  void *allocated = nullptr, *aligned = nullptr;
  std::int64_t offset = 0, count = 2, stride = 1;
};
static_assert(sizeof(Callable) == 48 && sizeof(Payload) == 16 && sizeof(View) == 40 &&
              sizeof(Arguments) == 88);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(Invocation &context) {
  require(!context.callDepth(), "nested transfer leaked call admission");
  context.settle();
  require(!context.live() && !context.environmentBytes() &&
              context.allocations() == context.releases(),
          "nested transfer leaked owners/reservations");
}
static Payload wrap(Invocation &source, Payload inner) {
  void *environment = source.allocateEnvironment(sizeof(Payload), kalandra_nested_test_outer);
  require(environment, "nested environment allocation failed");
  std::memcpy(environment, &inner, sizeof inner);
  return {kalandra_nested_test_outer, environment};
}
static Callable make(Invocation &source, unsigned depth = 1) {
  auto *values = static_cast<std::int64_t *>(source.allocateRecord(16));
  require(values, "nested data allocation failed");
  values[0] = values[1] = 20;
  View view;
  view.allocated = view.aligned = values;
  void *environment = source.allocateEnvironment(sizeof(View), kalandra_nested_test_base);
  require(environment, "base environment allocation failed");
  std::memcpy(environment, &view, sizeof view);
  Payload value{kalandra_nested_test_base, environment};
  for (unsigned i = 0; i < depth; ++i)
    value = wrap(source, value);
  return {{}, value};
}
using Invoke = std::int64_t (*)(void *, std::int64_t, Invocation *);
int main() {
  try {
    const auto &layout = *kalandra_nested_test_layout;
    const auto &entry = *kalandra_nested_test_entry;
    require(layout.bytes == sizeof(Callable) && entry.packetBytes == sizeof(Arguments),
            "private nested ABI changed");
    auto parent = std::make_unique<Invocation>();
    auto original = make(*parent, 16);
    Invocation first, second;
    Callable a, b;
    require(layout.copy(first, &a, *parent, &original) &&
                layout.copy(second, &b, *parent, &original),
            "nested copy failed");
    require(a.payload.environment != b.payload.environment &&
                a.payload.environment != original.payload.environment,
            "nested environment storage shared unexpectedly");
    require(first.environmentBytes() == 16 * 16 + 40 && second.environmentBytes() == 16 * 16 + 40,
            "nested environment accounting changed");
    balanced(*parent);
    parent.reset();
    auto invoke = reinterpret_cast<Invoke>(kalandra_nested_test_outer);
    for (unsigned i = 0; i < 128; ++i)
      require(invoke(a.payload.environment, 2, &first) == 42 && !first.status(),
              "nested capture lost lifetime");
    balanced(first);
    require(invoke(b.payload.environment, 2, &second) == 42 && !second.status(),
            "settled sibling invalidated nested capture");
    balanced(second);

    Invocation source;
    auto valid = make(source);
    for (unsigned mode = 0; mode < 10; ++mode) {
      auto input = valid;
      auto inner = *static_cast<Payload *>(valid.payload.environment);
      if (mode == 0)
        inner.code = nullptr;
      if (mode == 1)
        inner.code = reinterpret_cast<void *>(1);
      if (mode == 2)
        inner.environment = reinterpret_cast<void *>(1);
      if (mode == 3)
        inner.environment = static_cast<std::byte *>(inner.environment) + 1;
      if (mode == 4)
        inner.environment = source.allocateEnvironment(16, kalandra_nested_test_base);
      if (mode == 5)
        inner.environment = source.allocateEnvironment(40, kalandra_nested_test_outer);
      if (mode == 6) {
        auto view = *static_cast<View *>(inner.environment);
        view.count = INT64_MAX;
        inner.environment = source.allocateEnvironment(40, kalandra_nested_test_base);
        std::memcpy(inner.environment, &view, sizeof view);
      }
      input.payload = wrap(source, inner);
      if (mode == 7) {
        // A forged cycle must fail active-path validation, without native
        // stack overflow or publication of a partially copied graph.
        std::memcpy(input.payload.environment, &input.payload, sizeof input.payload);
      }
      Invocation destination;
      std::atomic<bool> cancelled{true};
      if (mode == 8)
        destination.fault(4, 8, 9);
      if (mode == 9)
        destination.bindCancellation(&cancelled);
      alignas(Callable) std::array<std::byte, sizeof(Callable)> output;
      output.fill(std::byte{0x5a});
      const auto before = output;
      require(!layout.copy(destination, output.data(), source, &input) && output == before,
              "invalid nested copy published output");
      require(destination.status() == (mode == 7   ? 3
                                       : mode == 8 ? 4
                                       : mode == 9 ? 0
                                                   : 3),
              "nested copy changed primary failure");
      balanced(destination);
    }
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
      Invocation destination;
      destination.failAllocationAt(attempt);
      Callable output;
      const auto ok = layout.copy(destination, &output, source, &valid);
      require(attempt >= 1 && attempt <= 3 ? !ok && destination.status() == 2
                                           : ok && !destination.status(),
              "nested allocation probe changed result");
      // These probes target copy allocation. Invocation has its own checked
      // frame allocations, so stop injecting before evaluating the result.
      destination.failAllocationAt(0);
      if (ok)
        require(destination.environmentBytes() == 56 &&
                    invoke(output.payload.environment, 2, &destination) == 42,
                "nested allocation probe lost value");
      balanced(destination);
    }
    Invocation admitted;
    for (unsigned n = 0; n < 4096; ++n)
      require(admitted.enterCall(0, 0), "test admission failed");
    Callable denied;
    require(layout.copy(admitted, &denied, source, &valid) && admitted.status() == 0 &&
                admitted.callDepth() == 4096,
            "nested copy retained a logical call cap or unwound caller frames");
    for (unsigned n = 0; n < 4096; ++n)
      admitted.leaveCall();
    balanced(admitted);

    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "nested core allocation failed");
      auto group = core->openRoot();
      auto producer = std::make_unique<Invocation>();
      Arguments args;
      args.value.value = 2;
      args.function = make(*producer, 4);
      std::vector<TaskHandle> handles;
      for (unsigned n = 0; n < 12; ++n) {
        auto task =
            core->submit(group, entry, {entry.packetLayout, &args, sizeof args, producer.get()});
        require(bool(task), "nested task not admitted");
        handles.push_back(task);
      }
      require(core->stats().environmentBytes == 12 * (4 * 16 + 40),
              "nested task environment bytes not charged");
      balanced(*producer);
      producer.reset();
      require(core->close(group) && core->drain(), "nested tasks did not settle");
      Invocation observer;
      for (auto task : handles) {
        require(core->failure(task).kind == TaskFailure::None, "nested task failed");
        for (unsigned n = 0; n < 2; ++n) {
          Scalar value;
          require(core->observe(task, *entry.result, observer, &value) && !value.header.kind &&
                      value.value == 42,
                  "nested cached outcome changed");
        }
      }
      require(core->release(group), "nested group release failed");
      balanced(observer);
      auto stats = core->stats();
      require(!stats.environmentBytes && !stats.retainedBytes && !stats.retainedOwners &&
                  !stats.records && !stats.groups,
              "nested scheduler cleanup leaked");
      TaskLimits tiny;
      tiny.workers = workers;
      tiny.bytes = entry.frameBytes + entry.result->bytes + 55;
      auto quota = TaskCore::create(tiny);
      require(bool(quota), "nested quota core failed");
      auto qgroup = quota->openRoot();
      Arguments packet;
      packet.value.value = 2;
      packet.function = valid;
      auto task =
          quota->submit(qgroup, entry, {entry.packetLayout, &packet, sizeof packet, &source});
      require(bool(task) && quota->close(qgroup) && quota->drain(), "nested quota did not settle");
      require(quota->failure(task).kind == TaskFailure::Fault && quota->failure(task).code == 2,
              "nested environment quota ignored");
      require(quota->release(qgroup) && !quota->stats().retainedBytes &&
                  !quota->stats().environmentBytes,
              "nested quota leaked");
      auto stopped = TaskCore::create(limits);
      auto cgroup = stopped->openRoot();
      auto cancelled =
          stopped->submit(cgroup, entry, {entry.packetLayout, &packet, sizeof packet, &source});
      require(bool(cancelled) && stopped->cancel(cgroup) && stopped->close(cgroup) &&
                  stopped->drain(),
              "nested cancellation did not settle");
      require(stopped->failure(cancelled).kind == TaskFailure::Cancelled,
              "nested cancellation ignored");
      require(stopped->release(cgroup) && !stopped->stats().retainedBytes &&
                  !stopped->stats().environmentBytes,
              "nested cancellation leaked");
    }
    balanced(source);
    std::cout << "Nested worker closures: 129 post-creator calls; 10 invalid/stopped graphs; 5 "
                 "allocation probes; checked admission; 36 tasks/72 cached observations; quota and "
                 "cancellation passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
