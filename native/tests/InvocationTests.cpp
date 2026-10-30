#include "../runtime/Runtime.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <new>
#include <thread>
using kalandra::runtime::Invocation;
static void require(bool condition, int line) {
  if (!condition) {
    std::fprintf(stderr, "Invocation assertion at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(condition) require(condition, __LINE__)
static void coldLifecycle() {
  using kalandra::runtime::InvocationABI;
  // Reuse poisoned storage so accidentally reading the inactive ledger or
  // stale control state cannot pass just because stack memory happened to zero.
  alignas(Invocation) std::byte storage[sizeof(Invocation)];
  for (unsigned repeat = 0; repeat != 512; ++repeat) {
    std::memset(storage, 0xa5, sizeof(storage));
    auto *context = repeat & 1 ? ::new (storage) Invocation{} : ::new (storage) Invocation;
    CHECK(!InvocationABI::resourceLedgerMaterialized(*context));
    CHECK(storage[InvocationABI::ledgerStorage] == std::byte{0xa5});
    CHECK(!context->status() && !context->callDepth() && !context->line() && !context->column());
    CHECK(!context->live() && !context->allocations() && !context->releases());
    CHECK(!context->environmentBytes() && !context->recursiveBytes());
    CHECK(context->usable() && !context->hasExpectedError());
    CHECK(!context->lease(storage));
    std::atomic<bool> token{true};
    context->bindCancellation(&token);
    context->allowNativeInterrupts(true);
    CHECK(context->poll() == -1 && !context->status());
    token.store(false, std::memory_order_release);
    CHECK(!context->poll());
    CHECK(!InvocationABI::resourceLedgerMaterialized(*context));
    if (repeat % 3 == 0) {
      context->fault(4, 23, 11);
      context->fault(3, 99, 99);
      CHECK(context->status() == 4 && context->line() == 23 && context->column() == 11);
      std::atomic<std::uint64_t> polls{0};
      context->watchPolls(&polls);
      CHECK(context->poll() == 4 && polls.load() == 1);
      CHECK(context->status() == 4 && context->line() == 23 && context->column() == 11);
    } else if (repeat % 3 == 1) {
      context->failAllocationAt(1);
      CHECK(!context->allocate(8) && context->status() == 2);
      CHECK(InvocationABI::resourceLedgerMaterialized(*context));
    } else {
      context->settle();
      CHECK(!context->status() && !context->live());
      CHECK(context->executionStatus() == 3);
      CHECK(!InvocationABI::resourceLedgerMaterialized(*context));
    }
    context->settle();
    context->settle();
    CHECK(!context->live() && context->allocations() == context->releases());
    context->~Invocation();
  }
}
int main(int argc, char **) {
  if (argc != 2) coldLifecycle();
  if (argc == 2) {
    // CTest supplies FAIL_ALLOC_AT=2 in a fresh process. Scalar work does not
    // consume an allocation attempt; explicit zero disables the process hook.
    Invocation injected, disabled, overridden;
    CHECK(injected.enterCall(0, 0));
    injected.leaveCall();
    CHECK(injected.allocate(8));
    CHECK(!injected.allocate(8) && injected.status() == 2);
    injected.settle();
    injected.settle();
    CHECK(injected.live() == 0 && injected.allocations() == injected.releases());
    Invocation second;
    CHECK(second.allocate(8));
    CHECK(!second.allocate(8) && second.status() == 2);
    second.settle();
    CHECK(second.allocations() == second.releases());
    disabled.failAllocationAt(0);
    CHECK(disabled.allocate(8) && disabled.allocate(8));
    overridden.failAllocationAt(1);
    CHECK(!overridden.allocate(8) && overridden.status() == 2);
    return 0;
  }
  {
    // The live diagnostic remains safe to sample during first materialization.
    Invocation context;
    std::atomic<bool> stop{false};
    std::thread reader([&] {
      while (!stop.load(std::memory_order_acquire)) CHECK(context.live() <= 1);
    });
    for (unsigned i = 0; i != 1000; ++i) {
      auto *buffer = context.allocate(8);
      CHECK(buffer);
      context.release(buffer);
    }
    context.settle();
    stop.store(true, std::memory_order_release);
    reader.join();
  }
  for (unsigned repeat = 0; repeat < 1000; ++repeat) {
    Invocation parent, child, sibling, result;
    auto *values = static_cast<float *>(parent.allocate(2 * sizeof(float)));
    CHECK(values);
    CHECK(reinterpret_cast<std::uintptr_t>(values) % alignof(std::max_align_t) == 0);
    values[0] = 12.0f;
    values[1] = -3.0f;
    CHECK(child.retainFrom(parent, values));
    CHECK(sibling.retainFrom(parent, values));
    auto *temporary = child.allocate(128);
    CHECK(temporary);
    child.fault(1, 7, 9);
    child.fault(2, 8, 10);
    CHECK(child.status() == 1 && child.line() == 7 && child.column() == 9);
    CHECK(child.allocate(1) == nullptr);
    child.settle();
    child.settle();
    CHECK(child.live() == 0 && child.allocations() == child.releases());
    CHECK(parent.live() == 1 && sibling.live() == 1);
    CHECK(values[0] == 12.0f && values[1] == -3.0f);
    // Publish a completed result before destroying both original owners.
    CHECK(result.retainFrom(sibling, values));
    parent.settle();
    sibling.settle();
    CHECK(values[0] == 12.0f && values[1] == -3.0f);
    CHECK(result.status() == 0 && result.live() == 1);
    result.release(values);
    CHECK(result.live() == 0 && result.allocations() == result.releases());
  }
  // Self-retains and duplicate argument owners are released independently.
  {
    Invocation context;
    auto *value = static_cast<float *>(context.allocate(sizeof(float)));
    CHECK(value);
    *value = 42.0f;
    for (unsigned i = 0; i < 10000; ++i)
      CHECK(context.retainFrom(context, value));
    for (unsigned i = 0; i < 10000; ++i)
      context.release(value);
    CHECK(context.live() == 1 && *value == 42.0f);
    context.settle();
    CHECK(context.allocations() == context.releases());
  }
  for (std::uint64_t fail = 1; fail <= 3; ++fail) {
    Invocation parent, child;
    auto *data = parent.allocate(4);
    CHECK(data);
    child.failAllocationAt(fail);
    bool completed = child.allocate(8) && child.retainFrom(parent, data) && child.allocate(16);
    CHECK(!completed && child.status() == 2);
    child.settle();
    CHECK(child.live() == 0 && child.allocations() == fail - 1);
    CHECK(child.allocations() == child.releases());
    CHECK(parent.live() == 1 && parent.status() == 0);
  }
  {
    Invocation large;
    CHECK(!large.allocate(std::numeric_limits<std::uint64_t>::max()));
    CHECK(large.status() == 2 && large.live() == 0);
  }
  {
    Invocation parent, wrong, foreign;
    auto *value = parent.allocate(0);
    CHECK(value);
    wrong.release(value);
    CHECK(wrong.status() == 3 && parent.live() == 1);
    CHECK(!foreign.retainFrom(wrong, value));
    CHECK(foreign.status() == 3 && parent.live() == 1);
    parent.release(value);
    parent.release(value);
    CHECK(parent.status() == 3 && parent.live() == 0);
  }
  {
    Invocation closed;
    closed.settle();
    CHECK(kalandra_status_v1(&closed) == 3);
    CHECK(closed.allocate(1) == nullptr);
  }
  std::puts("Invocation isolation, retained results, duplicate owners, fault latching, and "
            "allocation failures pass");
}
