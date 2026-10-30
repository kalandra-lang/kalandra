#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace kalandra::runtime;
extern "C" {
extern const TaskEntry *const kalandra_result_test_entry;
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
static_assert(sizeof(Callable) == 48 && sizeof(Scalar) == 40);
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static void balanced(Invocation &ctx) {
  require(!ctx.callDepth(), "result copy leaked call admission");
  ctx.settle();
  require(!ctx.live() && !ctx.environmentBytes() && ctx.allocations() == ctx.releases(),
          "result copy leaked owners/reservations");
}
using Invoke = std::int64_t (*)(void *, std::int64_t, Invocation *);
static TaskCore *active = nullptr;
static GroupHandle activeGroup;
static const ValueLayout *original = nullptr;
static unsigned mode = 0;
static bool intercept(Invocation &dst, void *out, const Invocation &src, const void *in) noexcept {
  if (mode == 0)
    active->cancel(activeGroup);
  if (mode == 2 || mode == 3)
    dst.failAllocationAt(mode - 1);
  const bool ok = original->copy(dst, out, src, in);
  if (mode == 1)
    active->cancel(activeGroup);
  return ok;
}
int main() {
  try {
    const auto &entry = *kalandra_result_test_entry;
    require(entry.packetBytes == sizeof(Scalar) && entry.result->bytes == sizeof(Callable),
            "private result ABI changed");
    for (unsigned workers : {1u, 2u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      require(bool(core), "result core creation failed");
      auto group = core->openRoot();
      Invocation source;
      Scalar args;
      std::vector<TaskHandle> handles;
      for (unsigned n = 0; n < 12; ++n) {
        auto task = core->submit(group, entry, {entry.packetLayout, &args, sizeof args, &source});
        require(bool(task), "result task admission failed");
        handles.push_back(task);
      }
      balanced(source);
      require(core->close(group) && core->drain(), "result tasks did not settle");
      require(core->stats().environmentBytes == 12 * 40, "cached outcome environments not charged");
      Invocation observer;
      std::vector<Callable> values;
      for (auto task : handles) {
        require(core->failure(task).kind == TaskFailure::None, "closure producer failed");
        Callable first, second;
        require(core->observe(task, *entry.result, observer, &first) &&
                    core->observe(task, *entry.result, observer, &second),
                "cached function observation failed");
        require(first.code == second.code && first.environment != second.environment,
                "cached observations shared environment storage");
        values.push_back(first);
        values.push_back(second);
      }
      for (unsigned attempt : {1u, 2u}) {
        Invocation denied;
        denied.failAllocationAt(attempt);
        alignas(Callable) std::array<std::byte, sizeof(Callable)> out;
        out.fill(std::byte{0x5a});
        const auto before = out;
        require(!core->observe(handles[0], *entry.result, denied, out.data()) &&
                    denied.status() == 2 && out == before,
                "failed observation published a closure");
        balanced(denied);
      }
      Invocation cancelled;
      std::atomic<bool> stop{true};
      cancelled.bindCancellation(&stop);
      Callable untouched;
      require(!core->observe(handles[0], *entry.result, cancelled, &untouched) &&
                  !cancelled.status(),
              "cancelled observation became an internal fault");
      balanced(cancelled);
      require(core->release(group), "result group release failed");
      const auto stats = core->stats();
      require(!stats.environmentBytes && !stats.retainedBytes && !stats.records && !stats.groups &&
                  !stats.retainedOwners,
              "cached result cleanup leaked");
      core.reset();
      for (const auto &value : values) {
        auto call = reinterpret_cast<Invoke>(value.code);
        for (unsigned n = 0; n < 8; ++n)
          require(call(value.environment, 2, &observer) == 42 && !observer.status(),
                  "returned closure died with its task/group/core");
      }
      balanced(observer);
      TaskLimits tiny;
      tiny.workers = workers;
      tiny.bytes = entry.frameBytes + entry.result->bytes + 39;
      auto quota = TaskCore::create(tiny);
      require(bool(quota), "result quota core failed");
      auto qgroup = quota->openRoot();
      Invocation qsource;
      auto task = quota->submit(qgroup, entry, {entry.packetLayout, &args, sizeof args, &qsource});
      require(bool(task) && quota->close(qgroup) && quota->drain(),
              "result quota failed to settle");
      require(quota->failure(task).kind == TaskFailure::Fault && quota->failure(task).code == 2,
              "cached outcome bypassed root quota");
      require(quota->release(qgroup) && !quota->stats().retainedBytes &&
                  !quota->stats().environmentBytes,
              "result quota leaked");
      balanced(qsource);
    }
    original = entry.result;
    for (mode = 0; mode < 4; ++mode) {
      auto core = TaskCore::create();
      require(bool(core), "intercept core failed");
      active = core.get();
      activeGroup = core->openRoot();
      auto intercepted = *original;
      intercepted.copy = intercept;
      auto target = entry;
      target.result = &intercepted;
      Invocation source;
      Scalar args;
      auto task =
          core->submit(activeGroup, target, {target.packetLayout, &args, sizeof args, &source});
      require(bool(task) && core->close(activeGroup) && core->drain(),
              "intercept outcome failed to settle");
      auto failure = core->failure(task);
      require(mode < 2 ? failure.kind == TaskFailure::Cancelled
                       : failure.kind == TaskFailure::Fault && failure.code == 2,
              "outcome copy changed cancellation/allocation failure");
      require(core->release(activeGroup) && !core->stats().retainedBytes &&
                  !core->stats().environmentBytes,
              "intercept outcome leaked");
      balanced(source);
      active = nullptr;
    }
    std::cout << "Worker function results: 36 tasks/72 observations; 576 post-core calls; 6 failed "
                 "and 3 cancelled observations; 3 quotas; 2 outcome cancellation and 2 outcome "
                 "allocation failures passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
