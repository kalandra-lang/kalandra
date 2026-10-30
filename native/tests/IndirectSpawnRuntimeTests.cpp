#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_indirect_factory;
extern const TaskEntry *const kalandra_indirect_child;
}
struct Header {
  std::int32_t kind = 0;
  std::int64_t code = 0, line = 0, column = 0;
};
struct Scalar {
  Header header;
  std::int64_t value = 40;
};
struct Callable {
  Header header;
  void *code = nullptr, *environment = nullptr;
};
struct Arguments {
  Callable callee;
  Scalar value{{}, 2};
};
static_assert(sizeof(Scalar) == 40 && sizeof(Callable) == 48 && sizeof(Arguments) == 88);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(Invocation &ctx) {
  require(!ctx.callDepth(), "indirect handoff leaked checked call admission");
  ctx.settle();
  require(!ctx.live() && !ctx.environmentBytes() && ctx.allocations() == ctx.releases(),
          "indirect handoff leaked storage/reservations");
}
int main() {
  try {
    const auto &factory = *kalandra_indirect_factory;
    const auto &child = *kalandra_indirect_child;
    require(factory.packetBytes == sizeof(Scalar) && factory.result->bytes == sizeof(Callable) &&
                child.packetBytes == sizeof(Arguments) && child.result->bytes == sizeof(Scalar) &&
                child.frameAlignment <= alignof(std::max_align_t),
            "private indirect spawn ABI changed");
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto producer = TaskCore::create(limits);
      require(bool(producer), "producer core unavailable");
      auto group = producer->openRoot();
      Invocation input, source;
      Scalar offset;
      auto made =
          producer->submit(group, factory, {factory.packetLayout, &offset, sizeof offset, &input});
      require(bool(made) && producer->close(group) && producer->drain(), "factory failed");
      balanced(input);
      Arguments arguments;
      require(producer->failure(made).kind == TaskFailure::None &&
                  producer->observe(made, *factory.result, source, &arguments.callee),
              "factory function observation failed");
      require(producer->release(group) && !producer->stats().retainedBytes &&
                  !producer->stats().environmentBytes,
              "factory storage stayed live after release");
      producer.reset();

      const auto original = arguments;
      std::vector<std::max_align_t> frame((child.frameBytes + sizeof(std::max_align_t) - 1) /
                                          sizeof(std::max_align_t));
      for (unsigned mode = 0; mode < 7; ++mode) {
        auto invalid = arguments;
        if (mode == 0)
          invalid.callee.code = nullptr;
        if (mode == 1)
          invalid.callee.code = reinterpret_cast<void *>(1);
        if (mode == 2)
          invalid.callee.environment = reinterpret_cast<void *>(1);
        if (mode == 3)
          invalid.callee.environment = static_cast<std::byte *>(invalid.callee.environment) + 1;
        if (mode == 4)
          invalid.callee.environment = source.allocateEnvironment(40, reinterpret_cast<void *>(1));
        Invocation destination;
        std::atomic<bool> stopped{true};
        if (mode == 5)
          destination.fault(4, 8, 9);
        if (mode == 6)
          destination.bindCancellation(&stopped);
        require(!child.construct(destination, frame.data(),
                                 {child.packetLayout, &invalid, sizeof invalid, &source}),
                "invalid/stopped indirect frame admitted");
        require(destination.status() == (mode == 5   ? 4
                                         : mode == 6 ? 0
                                                     : 3),
                "indirect frame replaced primary failure");
        balanced(destination);
      }
      unsigned denied = 0, admitted = 0;
      for (unsigned attempt = 1; attempt <= 8; ++attempt) {
        Invocation destination;
        destination.failAllocationAt(attempt);
        const bool ok = child.construct(
            destination, frame.data(), {child.packetLayout, &arguments, sizeof arguments, &source});
        require(ok ? !destination.status() : destination.status() == 2,
                "indirect allocation probe changed failure kind");
        ok ? ++admitted : ++denied;
        balanced(destination);
      }
      require(admitted && denied && std::memcmp(&arguments, &original, sizeof arguments) == 0,
              "indirect copy changed source or skipped allocation probes");

      auto consumer = TaskCore::create(limits);
      require(bool(consumer), "consumer core unavailable");
      auto receiving = consumer->openRoot();
      std::vector<TaskHandle> tasks;
      for (unsigned n = 0; n < 12; ++n) {
        auto task = consumer->submit(receiving, child,
                                     {child.packetLayout, &arguments, sizeof arguments, &source});
        require(bool(task), "indirect task admission failed");
        tasks.push_back(task);
      }
      // Every child owns a copy before the submitting invocation is released.
      balanced(source);
      require(consumer->close(receiving) && consumer->drain(),
              "indirect children failed to settle");
      Invocation observer;
      for (auto task : tasks) {
        Scalar a, b;
        require(consumer->failure(task).kind == TaskFailure::None &&
                    consumer->observe(task, *child.result, observer, &a) &&
                    consumer->observe(task, *child.result, observer, &b) && !a.header.kind &&
                    !b.header.kind && a.value == 42 && b.value == 42,
                "indirect callback lost captured data/cached result");
      }
      balanced(observer);
      require(consumer->release(receiving) && !consumer->stats().environmentBytes &&
                  !consumer->stats().retainedBytes && !consumer->stats().records &&
                  !consumer->stats().groups && !consumer->stats().unfinished,
              "indirect tasks leaked scheduler storage");
    }
    std::cout << "Indirect spawn: 36 post-creator tasks, 72 observations, "
                 "21 invalid/stopped frames and 24 allocation probes passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
