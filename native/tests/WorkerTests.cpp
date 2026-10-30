#include "../runtime/Tasks.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
using namespace kalandra::runtime;
static void check(bool condition, int line) {
  if (!condition) {
    std::fprintf(stderr, "Worker assertion at line %d\n", line);
    std::exit(1);
  }
}
#define CHECK(x) check(bool(x), __LINE__)
template <class Predicate> static void until(Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready()) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}
struct Value {
  float number;
  TaskCore *core;
};
static bool copyValue(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  const auto value = *static_cast<const Value *>(in);
  (void)value.core->stats(); // Reentry proves callbacks do not run under the scheduler lock.
  *static_cast<Value *>(out) = value;
  return true;
}
static void destroyValue(void *in) noexcept { (void)static_cast<Value *>(in)->core->stats(); }
static const ValueLayout layout{77, sizeof(Value), alignof(Value), copyValue, destroyValue};
enum class Mode { Plain, Overlap, YieldLease, CompleteLease, CleanupLease, Poll, Fault, Migrate };
struct Shared {
  TaskCore *core;
  Mode mode = Mode::Plain;
  std::atomic<unsigned> entered{0}, runs{0}, destroyed{0};
  std::atomic<bool> held{false}, release{false}, probeDone{false}, acknowledged{false};
};
struct Input {
  Shared *shared;
  float value = 1;
  int fault = 0;
};
struct Frame {
  Input input;
  unsigned phase = 0;
  float *storage = nullptr;
};
static bool construct(Invocation &, void *out, const Packet &packet) {
  auto input = *static_cast<const Input *>(packet.data);
  (void)input.shared->core->stats();
  CHECK(!input.shared->core->drain(0) && !input.shared->core->step());
  new (out) Frame{input};
  return true;
}
static void destroy(void *out) noexcept {
  auto *shared = static_cast<Frame *>(out)->input.shared;
  (void)shared->core->stats();
  if (shared->mode == Mode::CleanupLease) {
    shared->held = true;
    until([&] { return shared->release.load(); });
  }
  ++shared->destroyed;
}
static void resume(Task &task) {
  auto &f = *static_cast<Frame *>(task.frame());
  auto &s = *f.input.shared;
  ++s.runs;
  if (s.mode == Mode::Overlap || s.mode == Mode::Fault) {
    ++s.entered;
    until([&] { return s.entered.load() == 2; });
    if (s.mode == Mode::Fault) {
      task.fault(f.input.fault);
      return;
    }
  } else if (s.mode == Mode::YieldLease || s.mode == Mode::CompleteLease) {
    if (f.phase++ == 0) {
      const Value value{f.input.value, s.core};
      if (s.mode == Mode::YieldLease)
        task.yield();
      else
        task.complete(&value);
      s.held = true;
      until([&] { return s.release.load(); });
      s.held = false;
      return;
    }
    CHECK(!s.held.load());
  } else if (s.mode == Mode::Poll) {
    CHECK(task.context().allocate(32));
    ++s.entered;
    while (!task.context().poll())
      std::this_thread::yield();
    s.acknowledged = true;
    return;
  } else if (s.mode == Mode::Migrate) {
    if (!f.phase++) {
      f.storage = static_cast<float *>(task.context().allocate(sizeof(float)));
      CHECK(f.storage);
      *f.storage = f.input.value;
      task.yield();
      return;
    }
    CHECK(*f.storage == f.input.value);
  }
  const Value value{f.input.value, s.core};
  task.complete(&value);
}
static const TaskEntry entry{
    1,         88,      sizeof(Input), alignof(Input), sizeof(Frame), alignof(Frame),
    construct, destroy, resume,        &layout};
static Packet packet(const Input &in) { return {88, &in, sizeof(in)}; }
static void clean(TaskCore &core) {
  const auto stats = core.stats();
  CHECK(!stats.retainedBytes && !stats.retainedOwners && !stats.unfinished && !stats.records &&
        !stats.groups && !stats.ready && !stats.pending && !stats.maintenanceTasks);
}
static void observe(TaskCore &core, TaskHandle task, float expected) {
  Invocation destination;
  Value out{};
  CHECK(core.observe(task, layout, destination, &out) && out.number == expected);
}
static void overlap() {
  TaskLimits limits;
  limits.workers = 4;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), Mode::Overlap};
  Input input{&shared, 42};
  auto group = core->openRoot();
  auto a = core->submit(group, entry, packet(input));
  auto b = core->submit(group, entry, packet(input));
  CHECK(a && b && core->drain());
  CHECK(core->stats().peakRunning >= 2 && shared.runs == 2 && shared.destroyed == 2);
  observe(*core, a, 42);
  observe(*core, a, 42);
  observe(*core, b, 42);
  CHECK(core->close(group) && core->drain() && core->release(group));
  clean(*core);
}
// A second entry advances many times while the first callback is deliberately
// held after yielding/completing. Its frame must not resume or be destroyed yet.
static void probe(Task &task) {
  auto &f = *static_cast<Frame *>(task.frame());
  auto &s = *f.input.shared;
  until([&] { return s.held.load(); });
  if (++f.phase < 50) {
    task.yield();
    return;
  }
  s.probeDone = true;
  const Value value{0, s.core};
  task.complete(&value);
}
static const TaskEntry probeEntry{
    1,         88,      sizeof(Input), alignof(Input), sizeof(Frame), alignof(Frame),
    construct, destroy, probe,         &layout};
static void leases(Mode mode, unsigned records) {
  TaskLimits limits;
  limits.workers = 4;
  limits.records = records;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), mode};
  Input input{&shared, 7};
  auto group = core->openRoot();
  auto task = core->submit(group, entry, packet(input));
  auto other = core->submit(group, probeEntry, packet(input));
  std::jthread runner([&] { CHECK(core->drain()); });
  until([&] { return shared.probeDone.load(); });
  CHECK(shared.runs == 1 && !core->settled(task));
  until([&] { return core->settled(other); });
  CHECK(core->stats().maintenanceTasks == (mode == Mode::CompleteLease ? 1U : 0U));
  // Only the probe is allowed to have been destroyed so far.
  CHECK(shared.destroyed <= 1);
  shared.release = true;
  runner.join();
  CHECK(shared.runs == (mode == Mode::YieldLease ? 2u : 1u));
  observe(*core, task, 7);
  observe(*core, other, 0);
  CHECK(core->close(group) && core->drain() && core->release(group));
  clean(*core);
}
static void migration() {
  auto core = TaskCore::create();
  CHECK(core);
  Shared shared{core.get(), Mode::Migrate};
  Input input{&shared, 23};
  auto group = core->openRoot();
  auto task = core->submit(group, entry, packet(input));
  std::atomic<bool> firstDone{false}, leave{false};
  std::jthread first([&] {
    CHECK(core->step());
    firstDone = true;
    until([&] { return leave.load(); });
  });
  until([&] { return firstDone.load(); });
  std::jthread second([&] { CHECK(core->step()); });
  second.join();
  leave = true;
  first.join();
  CHECK(core->drain() && core->stats().migrations == 1);
  observe(*core, task, 23);
  CHECK(core->close(group) && core->drain() && core->release(group));
  clean(*core);
}
static void cancellation(bool deadline) {
  TaskLimits limits;
  limits.workers = 4;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), Mode::Poll}, good{core.get()};
  Input input{&shared}, other{&good, 17};
  auto group = core->openRoot(), independent = core->openRoot();
  auto task = core->submit(group, entry, packet(input));
  auto sibling = core->submit(independent, entry, packet(other));
  std::jthread runner([&] { CHECK(core->drain()); });
  until([&] { return shared.entered.load() == 1; });
  CHECK(core->stats().retainedOwners >= 1 && core->cancel(group, deadline));
  runner.join();
  CHECK(shared.acknowledged && core->settled(task) &&
        core->failure(task).kind == TaskFailure::Cancelled);
  CHECK(core->failure(group).kind ==
        (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
  observe(*core, sibling, 17);
  CHECK(core->close(independent) && core->drain());
  CHECK(core->release(group) && core->release(independent));
  clean(*core);
}
static void cancellationBeforePublication(Mode mode) {
  TaskLimits limits;
  limits.workers = 2;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), mode};
  Input input{&shared, 7};
  auto group = core->openRoot();
  auto task = core->submit(group, entry, packet(input));
  std::jthread runner([&] { CHECK(core->drain()); });
  until([&] { return shared.held.load(); });
  CHECK(!core->settled(task) && core->cancel(group));
  shared.release = true;
  runner.join();
  CHECK(core->failure(task).kind == TaskFailure::Cancelled &&
        core->failure(group).kind == TaskFailure::Cancelled);
  CHECK(core->release(group));
  clean(*core);
}
static void boundedDrain() {
  TaskLimits limits;
  limits.workers = 4;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), Mode::Migrate};
  Input input{&shared, 23};
  auto group = core->openRoot();
  auto task = core->submit(group, entry, packet(input));
  CHECK(!core->drain(0) && shared.runs == 0);
  CHECK(!core->drain(1) && shared.runs == 1 && core->stats().unfinished == 1);
  CHECK(core->drain());
  observe(*core, task, 23);
  CHECK(core->close(group) && core->drain() && core->release(group));
  clean(*core);
}
// A stopped budget must still wake its controller after the last execution
// or cleanup lease unwinds. Empty drains must also settle closed empty groups.
static void idleAndBudgetWakeups() {
  for (auto workers : {1u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (unsigned i = 0; i < 50; ++i) {
      CHECK(core->drain(0));
      auto group = core->openRoot();
      CHECK(core->close(group) && core->drain(0) && core->settled(group));
      CHECK(core->release(group));
    }
    clean(*core);
  }
  for (unsigned i = 0; i < 10; ++i)
    for (auto mode : {Mode::YieldLease, Mode::CompleteLease, Mode::CleanupLease}) {
      TaskLimits limits;
      limits.workers = 4;
      auto core = TaskCore::create(limits);
      CHECK(core);
      Shared shared{core.get(), mode};
      Input input{&shared, 19};
      auto group = core->openRoot();
      auto job = core->submit(group, entry, packet(input));
      std::atomic<bool> returned{false};
      bool drained = false;
      std::jthread controller([&] {
        drained = core->drain(1);
        returned = true;
      });
      until([&] { return shared.held.load(); });
      CHECK(!returned && !core->settled(job));
      // Other workers can exhaust the budget while this lease is still held.
      for (unsigned turn = 0; turn < 32; ++turn)
        std::this_thread::yield();
      shared.release = true;
      controller.join();
      CHECK(drained == (mode != Mode::YieldLease));
      CHECK(core->drain());
      observe(*core, job, 19);
      CHECK(core->close(group) && core->drain(0) && core->release(group));
      clean(*core);
    }
}
// Add work after the pool has parked spare workers around a running callback.
// New submissions must wake workers, not the blocked drain controller.
static void activeAdmissions() {
  for (unsigned repeat = 0; repeat < 10; ++repeat) {
    for (unsigned records : {4U, 128U}) {
      TaskLimits limits;
      limits.workers = 4;
      limits.records = records;
      auto core = TaskCore::create(limits);
      CHECK(core);
      Shared blocked{core.get(), Mode::Poll}, parallel{core.get(), Mode::Overlap};
      Input blocking{&blocked, 0}, input{&parallel, 71};
      auto root = core->openRoot();
      auto waiting = core->submit(root, entry, packet(blocking));
      std::jthread controller([&] { CHECK(core->drain()); });
      until([&] { return blocked.entered.load() == 1; });
      auto independent = core->openRoot();
      auto a = core->submit(independent, entry, packet(input));
      auto b = core->submit(independent, entry, packet(input));
      until([&] { return core->settled(a) && core->settled(b); });
      CHECK(parallel.entered == 2 && !core->settled(waiting));
      observe(*core, a, 71);
      observe(*core, b, 71);
      CHECK(core->close(independent));
      until([&] { return core->settled(independent); });
      CHECK(core->release(independent) && core->cancel(root));
      controller.join();
      CHECK(blocked.acknowledged && core->failure(waiting).kind == TaskFailure::Cancelled);
      CHECK(core->release(root));
      clean(*core);
    }
  }
}
// Three live callbacks yield behind a full one-slot ring. Returning leases
// must preserve every admitted continuation without expanding admission quota.
static void runnableOverflow() {
  for (unsigned repetition = 0; repetition < 10; ++repetition)
    for (unsigned action = 0; action < 3; ++action) {
      TaskLimits limits;
      limits.workers = 4;
      limits.records = 8;
      limits.groups = 4;
      limits.ready = 1;
      auto core = TaskCore::create(limits);
      CHECK(core);
      Shared a{core.get(), Mode::YieldLease}, b{core.get(), Mode::YieldLease},
          c{core.get(), Mode::YieldLease}, d{core.get()};
      Shared *shared[] = {&a, &b, &c, &d};
      auto root = core->openRoot();
      std::array<TaskHandle, 4> jobs;
      std::array<std::jthread, 3> leases;
      for (unsigned i = 0; i < 3; ++i) {
        Input input{shared[i], float(10 + i)};
        jobs[i] = core->submit(root, entry, packet(input));
        leases[i] = std::jthread([&] { CHECK(core->step()); });
        until([&] { return shared[i]->held.load(); });
      }
      Input last{&d, 13};
      jobs[3] = core->submit(root, entry, packet(last));
      for (unsigned i = 0; i < 3; ++i) {
        shared[i]->release = true;
        leases[i].join();
      }
      CHECK(core->stats().ready == 1 && core->stats().pending == 3);
      // A new submission still sees the full ready ring and cannot bypass it.
      auto rejected = core->openRoot();
      auto extra = core->submit(rejected, entry, packet(last));
      CHECK(core->failure(extra).kind == TaskFailure::AdmissionRejected);
      CHECK(core->stats().pending == 3);
      if (action == 2) {
        CHECK(core->cancel(root, true) && core->drain());
        CHECK(core->failure(root).kind == TaskFailure::DeadlineExceeded && !d.runs);
        for (auto job : jobs)
          CHECK(core->failure(job).kind == TaskFailure::Cancelled);
      } else {
        CHECK(core->step() && core->settled(jobs[3]));
        CHECK(a.runs == 1 && b.runs == 1 && c.runs == 1);
        CHECK(core->stats().ready == 1 && core->stats().pending == 2);
        observe(*core, jobs[3], 13);
        CHECK(core->step() && core->settled(jobs[0]) && b.runs == 1 && c.runs == 1);
        observe(*core, jobs[0], 10);
        if (action == 1)
          CHECK(core->cancel(root));
        CHECK(core->drain());
        for (unsigned i = 1; i < 3; ++i)
          if (action == 1)
            CHECK(core->failure(jobs[i]).kind == TaskFailure::Cancelled && shared[i]->runs == 1);
          else
            observe(*core, jobs[i], float(10 + i));
        if (!action)
          CHECK(core->close(root) && core->drain());
      }
      CHECK(core->release(root) && core->release(rejected));
      clean(*core);
      // Reuse the same slots after backlog cancellation/completion. Old handles
      // must not observe or schedule a new generation occupying those slots.
      auto fresh = core->openRoot();
      auto next = core->submit(fresh, entry, packet(last));
      Invocation stale;
      Value value{};
      CHECK(!core->observe(jobs[0], layout, stale, &value) && stale.status() == 3);
      CHECK(core->drain());
      observe(*core, next, 13);
      CHECK(core->close(fresh) && core->drain() && core->release(fresh));
      clean(*core);
    }
}
static void racingFaults() {
  TaskLimits limits;
  limits.workers = 2;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get(), Mode::Fault};
  Input a{&shared, 0, 1}, b{&shared, 0, 3};
  auto group = core->openRoot();
  CHECK(core->submit(group, entry, packet(a)));
  CHECK(core->submit(group, entry, packet(b)));
  CHECK(core->drain());
  const auto failure = core->failure(group);
  CHECK(failure.kind == TaskFailure::Fault && (failure.code == 1 || failure.code == 3));
  CHECK(core->release(group));
  clean(*core);
}
struct NestedInput {
  TaskCore *core;
  unsigned depth;
};
struct Nested {
  NestedInput input;
  unsigned phase = 0;
  GroupHandle group{};
  TaskHandle a{}, b{};
  float first = 0, second = 0;
};
static bool constructNested(Invocation &, void *out, const Packet &packet) {
  new (out) Nested{*static_cast<const NestedInput *>(packet.data)};
  return true;
}
static void nothing(void *) noexcept {}
static const TaskEntry &nestedEntry();
static void nestedResume(Task &task) {
  auto &f = *static_cast<Nested *>(task.frame());
  if (!f.input.depth) {
    Value v{1, f.input.core};
    task.complete(&v);
    return;
  }
  if (!f.phase) {
    f.group = task.openGroup();
    CHECK(f.group);
    NestedInput left{f.input.core, f.input.depth - 1}, right{f.input.core, 0};
    f.a = task.spawn(f.group, nestedEntry(), {99, &left, sizeof(left)});
    f.b = task.spawn(f.group, nestedEntry(), {99, &right, sizeof(right)});
    CHECK(f.a && f.b);
    f.phase = 1;
  }
  Value value{};
  if (f.phase == 1) {
    if (!task.await(f.a))
      return;
    CHECK(task.read(f.a, layout, &value));
    f.first = value.number;
    f.phase = 2;
  }
  if (f.phase == 2) {
    if (!task.await(f.b))
      return;
    CHECK(task.read(f.b, layout, &value));
    f.second = value.number;
    CHECK(task.close(f.group));
    f.phase = 3;
  }
  if (!task.join(f.group))
    return;
  CHECK(task.failure(f.group).kind == TaskFailure::None);
  const Value result{f.first + f.second, f.input.core};
  task.complete(&result);
}
static const TaskEntry &nestedEntry() {
  static const TaskEntry value{1,
                               99,
                               sizeof(NestedInput),
                               alignof(NestedInput),
                               sizeof(Nested),
                               alignof(Nested),
                               constructNested,
                               nothing,
                               nestedResume,
                               &layout};
  return value;
}
static void nestedProgress() {
  for (auto count : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = count;
    auto core = TaskCore::create(limits);
    CHECK(core);
    for (unsigned n = 0; n < 25; ++n) {
      GroupHandle groups[2];
      TaskHandle tasks[2][2];
      NestedInput input{core.get(), 4};
      for (unsigned i = 0; i < 2; ++i) {
        groups[i] = core->openRoot();
        for (auto &task : tasks[i])
          task = core->submit(groups[i], nestedEntry(), {99, &input, sizeof(input)});
      }
      CHECK(core->drain());
      for (unsigned i = 0; i < 2; ++i) {
        for (auto task : tasks[i]) {
          CHECK(core->failure(task).kind == TaskFailure::None);
          observe(*core, task, 5);
        }
        CHECK(core->close(groups[i]));
      }
      CHECK(core->drain());
      for (auto group : groups)
        CHECK(core->release(group));
      clean(*core);
    }
  }
}
struct Transfer {
  Shared *shared;
  float *backing;
};
static bool copyTransfer(Invocation &destination, void *out, const Invocation &source,
                         const void *in) noexcept {
  auto value = *static_cast<const Transfer *>(in);
  (void)value.shared->core->stats();
  if (value.shared->held) {
    value.shared->probeDone = true;
    until([&] { return value.shared->release.load(); });
  }
  if (!destination.retainFrom(source, value.backing))
    return false;
  *static_cast<Transfer *>(out) = value;
  return true;
}
static const ValueLayout transferLayout{101, sizeof(Transfer), alignof(Transfer), copyTransfer,
                                        nothing};
static void transferResume(Task &task) {
  auto &f = *static_cast<Frame *>(task.frame());
  auto *buffer = static_cast<float *>(task.context().allocate(sizeof(float)));
  CHECK(buffer);
  *buffer = 42;
  const Transfer value{f.input.shared, buffer};
  task.complete(&value);
}
static const TaskEntry transferEntry{
    1,         88,      sizeof(Input),  alignof(Input), sizeof(Frame), alignof(Frame),
    construct, destroy, transferResume, &transferLayout};
static void pinnedRead() {
  TaskLimits limits;
  limits.workers = 2;
  auto core = TaskCore::create(limits);
  CHECK(core);
  Shared shared{core.get()};
  Input input{&shared};
  auto group = core->openRoot();
  auto task = core->submit(group, transferEntry, packet(input));
  CHECK(core->drain());
  Invocation first;
  Transfer initial{};
  CHECK(core->observe(task, transferLayout, first, &initial));
  CHECK(core->close(group) && core->drain() && core->settled(group));
  shared.held = true;
  Invocation destination;
  Transfer result{};
  std::jthread reader([&] { CHECK(core->observe(task, transferLayout, destination, &result)); });
  until([&] { return shared.probeDone.load(); });
  CHECK(!core->release(group));
  shared.release = true;
  reader.join();
  CHECK(core->release(group));
  CHECK(*result.backing == 42 && *initial.backing == 42);
  clean(*core);
}
int main() {
  overlap();
  for (unsigned records : {4U, 128U})
    leases(Mode::YieldLease, records);
  for (unsigned records : {4U, 128U})
    leases(Mode::CompleteLease, records);
  migration();
  cancellation(false);
  cancellation(true);
  cancellationBeforePublication(Mode::CompleteLease);
  cancellationBeforePublication(Mode::CleanupLease);
  boundedDrain();
  idleAndBudgetWakeups();
  activeAdmissions();
  runnableOverflow();
  racingFaults();
  pinnedRead();
  nestedProgress();
  CHECK(!TaskCore::create(TaskLimits{.workers = 0}) &&
        !TaskCore::create(TaskLimits{.workers = 65}));
  std::puts("PASS concurrent callbacks, execution leases, migration, cancellation, failure latch, "
            "pinned outcomes, nested progress, and zero retained scheduler resources");
}
