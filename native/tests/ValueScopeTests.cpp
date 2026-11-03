#include "../runtime/ValueScopes.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Scope assertion %d: %s\n", __LINE__, #x);                              \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
using Scope = Invocation::OwnershipScope;
using Select = Invocation::OwnershipSelection;
struct Budget {
  std::uint64_t used = 0, peak = 0, limit = 16384;
  static bool reserve(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    if (n > b.limit - b.used)
      return false;
    b.used += n;
    b.peak = std::max(b.used, b.peak);
    return true;
  }
  static void release(void *p, std::uint64_t n) noexcept {
    auto &b = *static_cast<Budget *>(p);
    CHECK(n <= b.used);
    b.used -= n;
  }
};
static void bound(Invocation &ctx, Budget &b) {
  ctx.bindRecursiveBudget(&b, Budget::reserve, Budget::release);
}
struct Payload {
  void *data;
  std::size_t count;
};
static bool copyPayload(Invocation &dst, void *out, const Invocation &src,
                        const void *in) noexcept {
  auto value = *static_cast<const Payload *>(in);
  if (!dst.retainFrom(src, value.data))
    return false;
  *static_cast<Payload *>(out) = value;
  return true;
}
static void destroyPayload(void *) noexcept {}
static const ValueLayout layout{1, sizeof(Payload), alignof(Payload), copyPayload, destroyPayload};
static void valueTransactions() {
  Budget b;
  Invocation ctx;
  bound(ctx, b);
  const ValueLayout *layouts[] = {&layout, &layout, nullptr};
  auto *arena = ValueScopes::create(ctx, layouts);
  CHECK(arena && !ctx.lease(arena));
  Payload first{}, saved{};
  CHECK(arena->begin());
  first = {ctx.allocate(64), 64};
  CHECK(first.data);
  std::memset(first.data, 0x19, 64);
  CHECK(arena->commit(0, &first));
  saved = first;
  CHECK(arena->begin() && arena->commit(1, &saved));
  for (unsigned i = 0; i < 10000; ++i) {
    CHECK(arena->begin());
    first = {ctx.allocate(32 + (i % 3) * 32), 32 + (i % 3) * 32};
    CHECK(first.data);
    std::memset(first.data, i % 256, first.count);
    CHECK(arena->commit(0, &first));
    CHECK(static_cast<unsigned char *>(saved.data)[0] == 0x19);
    CHECK(static_cast<unsigned char *>(first.data)[0] == i % 256);
    CHECK(ctx.live() == 3 && b.used == ctx.recursiveBytes() && b.peak < 4096);
  }
  CHECK(arena->begin());
  CHECK(ctx.allocate(64));
  int scalar = 7;
  CHECK(arena->commit(2, &scalar) && scalar == 7 && ctx.live() == 3);
  arena->destroy();
  CHECK(!ctx.live());
  ctx.settle();
  CHECK(!b.used && ctx.allocations() == ctx.releases());
}

struct TaskInput {
  unsigned round = 0, rounds = 1, failAt = 0;
};
struct ScopedFrame {
  ValueScopes *arena = nullptr;
  Payload current{}, saved{};
  GroupHandle group;
  TaskHandle child;
  TaskInput input;
  unsigned phase = 0;
};
static bool childConstruct(Invocation &, void *out, const Packet &packet) {
  new (out) TaskInput(*static_cast<const TaskInput *>(packet.data));
  return true;
}
static void childDestroy(void *) noexcept {}
static void childResume(Task &task) {
  auto input = *static_cast<TaskInput *>(task.frame());
  Payload value{task.context().allocate(64), 64};
  if (!value.data)
    return;
  std::memset(value.data, input.round % 256, 64);
  task.complete(&value);
}
static const TaskEntry childEntry{1,
                                  9,
                                  sizeof(TaskInput),
                                  alignof(TaskInput),
                                  sizeof(TaskInput),
                                  alignof(TaskInput),
                                  childConstruct,
                                  childDestroy,
                                  childResume,
                                  &layout};
static const ValueLayout *taskLayouts[] = {&layout, &layout};
static bool scopedConstruct(Invocation &ctx, void *out, const Packet &packet) {
  auto *frame = new (out) ScopedFrame;
  frame->input = *static_cast<const TaskInput *>(packet.data);
  ctx.failAllocationAt(frame->input.failAt);
  frame->arena = ValueScopes::create(ctx, taskLayouts);
  return frame->arena != nullptr;
}
static void scopedDestroy(void *out) noexcept {
  auto &f = *static_cast<ScopedFrame *>(out);
  if (f.arena)
    f.arena->destroy();
}
static void scopedResume(Task &task) {
  auto &f = *static_cast<ScopedFrame *>(task.frame());
  if (f.phase == 0) {
    f.group = task.openGroup();
    f.child = task.spawn(f.group, childEntry, {9, &f.input, sizeof(f.input), &task.context()});
    if (!f.child)
      return;
    task.close(f.group);
    f.phase = 1;
  }
  if (f.phase == 1) {
    if (!task.await(f.child))
      return;
    if (task.failure(f.child).kind != TaskFailure::None) {
      task.fail(task.failure(f.child));
      return;
    }
    if (!f.arena->begin())
      return;
    if (!task.read(f.child, layout, &f.current)) {
      f.arena->end();
      return;
    }
    if (!f.arena->commit(0, &f.current))
      return;
    if (!f.input.round) {
      f.saved = f.current;
      if (!f.arena->begin() || !f.arena->commit(1, &f.saved))
        return;
    }
    CHECK(static_cast<unsigned char *>(f.saved.data)[0] == 0);
    CHECK(static_cast<unsigned char *>(f.current.data)[0] == f.input.round % 256);
    f.phase = 2;
  }
  if (!task.join(f.group))
    return;
  CHECK(task.releaseGroup(f.group));
  if (++f.input.round >= f.input.rounds) {
    task.complete(&f.current);
    return;
  }
  f.phase = 0;
  task.yield();
}
static const TaskEntry scopedEntry{1,
                                   10,
                                   sizeof(TaskInput),
                                   alignof(TaskInput),
                                   sizeof(ScopedFrame),
                                   alignof(ScopedFrame),
                                   scopedConstruct,
                                   scopedDestroy,
                                   scopedResume,
                                   &layout};
static void taskHandoffs() {
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 8;
    limits.groups = 4;
    limits.ready = 8;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    TaskInput input{0, 2000};
    auto task = core->submit(group, scopedEntry, {10, &input, sizeof(input)});
    CHECK(task);
    CHECK(core->drain(50000));
    CHECK(core->settled(task) && core->failure(task).kind == TaskFailure::None);
    Invocation destination;
    Payload output;
    CHECK(core->observe(task, layout, destination, &output));
    CHECK(static_cast<unsigned char *>(output.data)[0] == 1999 % 256);
    CHECK(core->close(group) && core->drain() && core->release(group));
    auto stats = core->stats();
    CHECK(!stats.retainedOwners && !stats.retainedBytes && !stats.environmentBytes &&
          !stats.recursiveBytes && !stats.records && !stats.groups);
    CHECK(static_cast<unsigned char *>(output.data)[0] == 1999 % 256);
    destination.settle();
  }
}

static void taskFailures() {
  for (unsigned workers : {1u, 4u})
    for (unsigned fail = 1; fail <= 14; ++fail) {
      TaskLimits limits;
      limits.workers = workers;
      limits.records = 8;
      limits.groups = 4;
      limits.ready = 8;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      TaskInput input{0, 4, fail};
      auto task = core->submit(group, scopedEntry, {10, &input, sizeof(input)});
      CHECK(task);
      CHECK(core->close(group) && core->drain(10000));
      auto result = core->failure(task);
      if (fail <= 10)
        CHECK(result.kind == TaskFailure::Fault && result.code == 2);
      else
        CHECK(result.kind == TaskFailure::None);
      CHECK(core->release(group));
      auto stats = core->stats();
      CHECK(!stats.retainedOwners && !stats.retainedBytes && !stats.recursiveBytes &&
            !stats.environmentBytes);
    }
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 8;
    limits.groups = 4;
    limits.ready = 8;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    TaskInput input{0, 10000};
    auto task = core->submit(group, scopedEntry, {10, &input, sizeof(input)});
    CHECK(task);
    CHECK(!core->drain(1));
    CHECK(core->cancel(group) && core->close(group) && core->drain(10000));
    CHECK(core->failure(task).kind == TaskFailure::Cancelled);
    CHECK(core->release(group));
    auto stats = core->stats();
    CHECK(!stats.retainedOwners && !stats.retainedBytes && !stats.recursiveBytes &&
          !stats.environmentBytes);
  }
}

static void nestedReturnPublication() {
  Budget budget;
  Invocation ctx;
  bound(ctx, budget);
  const ValueLayout *layouts[] = {&layout};
  auto *outer = ValueScopes::create(ctx, layouts);
  CHECK(outer);
  Payload current;
  for (unsigned round = 0; round < 10000; ++round) {
    CHECK(outer->begin());
    auto *inner = ValueScopes::create(ctx, layouts);
    CHECK(inner && inner->begin());
    Payload local{ctx.allocate(64), 64};
    CHECK(local.data);
    std::memset(local.data, round % 256, 64);
    CHECK(inner->commit(0, &local));
    // The parent scope has been restored; acquire the returned graph there
    // before destroying the callee frame and its ownership scope.
    CHECK(inner->publish(0, &current, &local));
    inner->destroy();
    CHECK(outer->commit(0, &current));
    CHECK(static_cast<unsigned char *>(current.data)[0] == round % 256);
    CHECK(ctx.live() == 2 && budget.peak < 4096);
  }
  outer->destroy();
  ctx.settle();
  CHECK(!ctx.live() && !budget.used && ctx.allocations() == ctx.releases());
}
static void closedRunnerPublication() {
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    limits.records = 8;
    limits.groups = 4;
    limits.ready = 8;
    auto runner = ClosedTaskRunner::create(limits);
    CHECK(runner);
    Invocation destination;
    Payload output;
    TaskInput input{0, 100};
    auto result =
        runner->run(scopedEntry, {10, &input, sizeof(input)}, layout, destination, &output);
    CHECK(result.kind == TaskFailure::None && static_cast<unsigned char *>(output.data)[0] == 99);
    auto stats = runner->stats();
    CHECK(!stats.retainedOwners && !stats.retainedBytes && !stats.recursiveBytes &&
          !stats.environmentBytes && !stats.records && !stats.groups);
    destination.settle();
  }
}

static void skippedNestedReturns() {
  for (bool rootSettlement : {false, true})
    for (bool fault : {false, true}) {
      Budget budget;
      Invocation ctx;
      bound(ctx, budget);
      const ValueLayout *layouts[] = {&layout};
      auto *outer = ValueScopes::create(ctx, layouts);
      CHECK(outer && outer->begin());
      auto *inner = ValueScopes::create(ctx, layouts);
      CHECK(inner && inner->begin());
      Payload value{ctx.allocate(64), 64};
      CHECK(value.data);
      std::memset(value.data, 0x71, 64);
      CHECK(inner->commit(0, &value));
      CHECK(inner->begin());
      CHECK(ctx.allocate(64));
      if (fault)
        ctx.fault(5, 7, 9);
      // Deliberately skip inner.destroy(), as a source abort can skip generated
      // synchronous return blocks. Parent/frame cleanup owns recovery.
      if (rootSettlement)
        ctx.settle();
      else
        outer->destroy();
      CHECK(!ctx.live() && !budget.used && ctx.allocations() == ctx.releases());
      CHECK(ctx.status() == (fault ? 5 : 0));
      ctx.settle();
    }
}
static void invalidAdmission() {
  {
    Invocation ctx;
    const ValueLayout *one = &layout;
    CHECK(!kalandra_value_scopes_create_v1(&ctx, &one, UINT64_MAX));
    CHECK(ctx.status() == 2 && !ctx.live() && !ctx.allocations());
  }

  kalandra_value_scopes_destroy_v1(nullptr);
  kalandra_value_scopes_end_v1(nullptr);
  CHECK(!kalandra_value_scopes_begin_v1(nullptr));
  CHECK(!kalandra_value_scopes_commit_v1(nullptr, 0, nullptr));
  {
    Invocation ctx;
    CHECK(!kalandra_value_scopes_create_v1(&ctx, nullptr, 1) && ctx.status() == 3);
    CHECK(!ctx.live());
  }
  {
    Invocation ctx;
    ValueLayout invalid = layout;
    invalid.alignment = 3;
    const ValueLayout *layouts[] = {&invalid};
    CHECK(!ValueScopes::create(ctx, layouts) && ctx.status() == 3 && !ctx.live());
  }
}
extern "C" [[noreturn]] void kalandra_source_abort_v2();
extern "C" bool kalandra_source_run_v3(Invocation *, bool (*)(void *), void *) noexcept;
struct AbortInput {
  Invocation *context;
  std::atomic<bool> *cancel;
  bool fault;
};
static bool abortBody(void *pointer) {
  auto &input = *static_cast<AbortInput *>(pointer);
  auto &ctx = *input.context;
  auto *outer = ValueScopes::create(ctx, taskLayouts);
  CHECK(outer && outer->begin());
  auto *inner = ValueScopes::create(ctx, taskLayouts);
  CHECK(inner && inner->begin() && ctx.allocate(64));
  if (input.fault)
    ctx.fault(5, 7, 9);
  else
    input.cancel->store(true);
  kalandra_source_abort_v2(); // Skip both generated return/cleanup paths.
}
static void sourceAbortCleanup() {
  for (bool fault : {false, true}) {
    Budget budget;
    Invocation ctx;
    std::atomic<bool> cancel = false;
    bound(ctx, budget);
    ctx.bindCancellation(&cancel);
    AbortInput input{&ctx, &cancel, fault};
    CHECK(!kalandra_source_run_v3(&ctx, abortBody, &input));
    CHECK(ctx.callDepth() == 0 && ctx.status() == (fault ? 5 : 0));
    ctx.settle();
    CHECK(!ctx.live() && !budget.used && ctx.allocations() == ctx.releases());
    CHECK(ctx.status() == (fault ? 5 : 0));
  }
}
int main() {
  sourceAbortCleanup();
  invalidAdmission();
  valueTransactions();
  taskHandoffs();
  taskFailures();
  nestedReturnPublication();
  closedRunnerPublication();
  skippedNestedReturns();
  std::puts("ValueScopes transactional, task, nested-return and skipped-return cleanup passed");
}
