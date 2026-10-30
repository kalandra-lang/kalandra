#include "../runtime/Tasks.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Closed task check %d\n", __LINE__);                                    \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static void nothing(void *) noexcept {}
static bool copyInt(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<int *>(out) = *static_cast<const int *>(in);
  return true;
}
static const ValueLayout integer{0x434c01, sizeof(int), alignof(int), copyInt, nothing};
struct Owned {
  std::shared_ptr<int> value;
};
static unsigned copies = 0, rejectCopyAt = 0;
static bool copyOwned(Invocation &context, void *out, const Invocation &, const void *in) noexcept {
  if (++copies == rejectCopyAt) {
    context.fault(17);
    return false;
  }
  new (out) Owned(*static_cast<const Owned *>(in));
  return true;
}
static void destroyOwned(void *value) noexcept { static_cast<Owned *>(value)->~Owned(); }
static const ValueLayout owned{0x434c02, sizeof(Owned), alignof(Owned), copyOwned, destroyOwned};
static bool constructOwned(Invocation &, void *out, const Packet &packet) {
  new (out) Owned(*static_cast<const Owned *>(packet.data));
  return true;
}
static void resumeOwned(Task &task) { task.complete(task.frame()); }
static const TaskEntry ownedEntry{1,
                                  203,
                                  sizeof(Owned),
                                  alignof(Owned),
                                  sizeof(Owned),
                                  alignof(Owned),
                                  constructOwned,
                                  destroyOwned,
                                  resumeOwned,
                                  &owned};
struct Shared {
  std::atomic<bool> probeDone{false}, busy{false}, eligible{true};
  ExternalCompletion completion;
};
struct Input {
  unsigned kind = 0, yields = 0;
  Shared *shared = nullptr;
};
struct Leaf {
  Input input;
  unsigned phase = 0;
};
static bool construct(Invocation &, void *out, const Packet &packet) {
  new (out) Leaf{*static_cast<const Input *>(packet.data)};
  return true;
}
static void resume(Task &task) {
  auto &f = *static_cast<Leaf *>(task.frame());
  auto &in = f.input;
  if (in.yields) {
    --in.yields;
    task.yield();
    return;
  }
  int value = 7;
  if (in.kind == 1 && !f.phase++) {
    in.shared->completion = task.suspendExternal();
    CHECK(in.shared->completion);
    return;
  }
  if (in.kind == 2) {
    in.shared->busy = true;
    while (!in.shared->probeDone.load())
      std::this_thread::yield();
  }
  if (in.kind == 0) {
    value = task.canExecuteSynchronously();
    if (in.shared) {
      in.shared->eligible = value;
      in.shared->probeDone = true;
    }
  }
  task.complete(&value);
}
static const TaskEntry leaf{
    1,         201,     sizeof(Input), alignof(Input), sizeof(Leaf), alignof(Leaf),
    construct, nothing, resume,        &integer};
static Packet packet(const Input &in) { return {201, &in, sizeof(in)}; }
struct ParentInput {
  unsigned mode;
  Shared *shared;
};
struct Parent {
  ParentInput input;
  unsigned phase = 0;
  GroupHandle group{};
  TaskHandle probe{}, other{};
  int value = -1;
};
static bool constructParent(Invocation &, void *out, const Packet &packet) {
  new (out) Parent{*static_cast<const ParentInput *>(packet.data)};
  return true;
}
static void resumeParent(Task &task) {
  auto &f = *static_cast<Parent *>(task.frame());
  if (!f.phase) {
    f.group = task.openGroup();
    CHECK(f.group);
    Input probe{0, 0, f.input.shared};
    if (f.input.mode == 2 || f.input.mode == 3) {
      Input other{f.input.mode - 1, 0, f.input.shared};
      f.other = task.spawn(f.group, leaf, packet(other));
      CHECK(f.other);
    }
    f.probe = task.spawn(f.group, leaf, packet(probe));
    CHECK(f.probe);
    if (f.input.mode == 1) {
      Input other{3};
      f.other = task.spawn(f.group, leaf, packet(other));
      CHECK(f.other);
    }
    f.phase = 1;
    if (f.input.mode == 4) {
      task.yield();
      return;
    }
  }
  if (f.phase == 1) {
    if (f.input.mode == 4)
      CHECK(task.cancel(f.group, true));
    if (!task.await(f.probe))
      return;
    const auto failure = task.failure(f.probe);
    if (failure.kind == TaskFailure::None)
      CHECK(task.read(f.probe, integer, &f.value));
    f.phase = 2;
  }
  if (f.phase == 2) {
    if (f.other) {
      if (!task.await(f.other))
        return;
      int ignored = 0;
      CHECK(task.read(f.other, integer, &ignored) && ignored == 7);
    }
    CHECK(task.close(f.group));
    f.phase = 3;
  }
  if (!task.join(f.group))
    return;
  const auto failure = task.failure(f.group);
  if (failure.kind != TaskFailure::None)
    task.fail(failure);
  else
    task.complete(&f.value);
}
static const TaskEntry parent{1,
                              202,
                              sizeof(ParentInput),
                              alignof(ParentInput),
                              sizeof(Parent),
                              alignof(Parent),
                              constructParent,
                              nothing,
                              resumeParent,
                              &integer};
static void empty(const ClosedTaskRunner &runner) {
  const auto s = runner.stats();
  CHECK(!s.groups && !s.records && !s.unfinished && !s.externalPending && !s.retainedBytes &&
        !s.retainedOwners);
}
int main() {
  // A public core can always be cancelled by its host, even when otherwise idle.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    Input in;
    auto child = core->submit(group, leaf, packet(in));
    CHECK(child && core->drain());
    Invocation context;
    int result = -1;
    CHECK(core->observe(child, integer, context, &result) && result == 0);
    CHECK(core->close(group) && core->drain() && core->release(group));
  }
  for (unsigned workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    auto runner = ClosedTaskRunner::create(limits);
    CHECK(runner);
    for (unsigned i = 0; i < 8; ++i) {
      Invocation context;
      Input in;
      int result = -1;
      CHECK(runner->run(leaf, packet(in), integer, context, &result).kind == TaskFailure::None);
      CHECK(result == 1);
      empty(*runner);
    }
  }
  for (unsigned mode = 0; mode < 5; ++mode) {
    TaskLimits limits;
    if (mode == 3)
      limits.workers = 2;
    auto runner = ClosedTaskRunner::create(limits);
    CHECK(runner);
    Shared shared;
    ParentInput in{mode, &shared};
    Invocation context;
    int result = -1;
    std::jthread signal;
    if (mode == 2)
      signal = std::jthread([&] {
        while (!shared.probeDone.load())
          std::this_thread::yield();
        CHECK(shared.completion.signal());
      });
    const auto failure = runner->run(parent, {202, &in, sizeof(in)}, integer, context, &result);
    if (mode == 4)
      CHECK(failure.kind == TaskFailure::DeadlineExceeded && !shared.eligible);
    else {
      CHECK(failure.kind == TaskFailure::None);
      CHECK(result == int(mode == 0));
    }
    empty(*runner);
  }
  // A successful entry may exceed the scheduler's per-drain step budget.
  {
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    Input in{0, 100005};
    Invocation context;
    int result = -1;
    CHECK(runner->run(leaf, packet(in), integer, context, &result).kind == TaskFailure::None);
    CHECK(result == 1 && runner->stats().resumes > 100000);
    empty(*runner);
  }
  {
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    unsigned failures = 0;
    bool succeeded = false;
    for (unsigned point = 1; point < 64; ++point) {
      runner->failAllocationAt(point);
      Invocation context;
      Input in;
      int result = -1;
      const auto failure = runner->run(leaf, packet(in), integer, context, &result);
      empty(*runner);
      if (failure.kind == TaskFailure::None) {
        CHECK(result == 1);
        succeeded = true;
        break;
      }
      CHECK(result == -1);
      ++failures;
    }
    CHECK(failures && succeeded);
    runner->failAllocationAt(0);
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < 4; ++i)
      threads.emplace_back([&] {
        for (unsigned j = 0; j < 32; ++j) {
          Invocation context;
          Input in;
          int result = -1;
          CHECK(runner->run(leaf, packet(in), integer, context, &result).kind == TaskFailure::None);
          CHECK(result == 1);
        }
      });
    threads.clear();
    empty(*runner);
  }
  {
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    Owned input{std::make_shared<int>(42)};
    std::weak_ptr<int> lifetime = input.value;
    alignas(Owned) std::byte slot[sizeof(Owned)];
    Invocation context;
    CHECK(runner->run(ownedEntry, {203, &input, sizeof(input)}, owned, context, slot).kind ==
          TaskFailure::None);
    empty(*runner);
    input.value.reset();
    runner.reset();
    auto *result = std::launder(reinterpret_cast<Owned *>(slot));
    CHECK(!lifetime.expired() && *result->value == 42 && result->value.use_count() == 1);
    destroyOwned(result);
    CHECK(lifetime.expired());
  }
  {
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    Owned input{std::make_shared<int>(42)};
    std::weak_ptr<int> lifetime = input.value;
    alignas(Owned) std::byte slot[sizeof(Owned)];
    Invocation context;
    copies = 0;
    rejectCopyAt = 2; // Outcome copy succeeds, observation into the caller fails.
    const auto failure =
        runner->run(ownedEntry, {203, &input, sizeof(input)}, owned, context, slot);
    CHECK(failure.kind == TaskFailure::Fault && failure.code == 17 && copies == 2);
    empty(*runner);
    input.value.reset();
    CHECK(lifetime.expired());
    rejectCopyAt = 0;
  }
  {
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    Input input;
    int result = -1;
    Invocation context;
    context.fault(91);
    const auto failure = runner->run(leaf, packet(input), integer, context, &result);
    CHECK(failure.kind == TaskFailure::Fault && failure.code == 91 && result == -1);
    CHECK(!runner->stats().resumes);
    empty(*runner);
    Invocation fresh;
    CHECK(runner->run(leaf, {201, &input, 0}, integer, fresh, &result).kind != TaskFailure::None);
    CHECK(result == -1);
    empty(*runner);
  }
  std::puts("Closed entry isolation, sibling/external rejection, source deadlines, long progress, "
            "allocation rollback and serialized callers pass");
}
