#include "../runtime/Tasks.h"
#include "../runtime/Time.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "time assertion line %d: %s\n", __LINE__, #x);                          \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
static TimeResult time(Invocation &ctx, TimeAction action, std::uint64_t input = 0) {
  TimeResult result;
  kalandra_time_v1(&ctx, &result, unsigned(action), input);
  return result;
}
static void nothing(void *) noexcept {}
static bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<std::uint64_t *>(out) = *static_cast<const std::uint64_t *>(in);
  return true;
}
static const ValueLayout layout{71, sizeof(std::uint64_t), alignof(std::uint64_t), copy, nothing};
struct Input {
  std::chrono::nanoseconds duration;
  std::atomic<unsigned> *started;
  std::atomic<unsigned> *finished;
  std::atomic<unsigned> *destroyed;
  bool fault = false;
};
struct Frame {
  Input input;
  unsigned phase = 0;
  Clock::time_point before;
};
static bool construct(Invocation &, void *out, const Packet &packet) {
  new (out) Frame{*static_cast<const Input *>(packet.data), 0, {}};
  return true;
}
static void destroy(void *address) noexcept {
  ++*static_cast<Frame *>(address)->input.destroyed;
  static_cast<Frame *>(address)->~Frame();
}
static void resume(Task &task) {
  auto &f = *static_cast<Frame *>(task.frame());
  if (task.cancelled())
    return;
  if (!f.phase++) {
    f.before = Clock::now();
    auto deadline = time(task.context(), TimeAction::Sleep, f.input.duration.count());
    CHECK(deadline.kind == TimeKind::Success);
    ++*f.input.started;
    kalandra_time_pause_v1(&task, deadline.value);
    return;
  }
  CHECK(Clock::now() - f.before >= f.input.duration);
  ++*f.input.finished;
  if (f.input.fault) {
    task.fault(1, 17, 3);
    return;
  }
  const std::uint64_t result = 42;
  task.complete(&result);
}
static const TaskEntry entry{
    1,         72,      sizeof(Input), alignof(Input), sizeof(Frame), alignof(Frame),
    construct, destroy, resume,        &layout};
static TaskHandle submit(TaskCore &core, GroupHandle group, const Input &input) {
  auto task = core.submit(group, entry, {72, &input, sizeof(input)});
  CHECK(task);
  return task;
}
static void awaitStarted(std::atomic<unsigned> &started, unsigned count) {
  const auto limit = Clock::now() + 10s;
  while (started.load() < count && Clock::now() < limit)
    std::this_thread::yield();
  CHECK(started.load() >= count);
}
static void timerCases(unsigned workers) {
  TaskLimits limits;
  limits.workers = workers;
  limits.records = 8;
  limits.ready = 2;
  limits.inFlight = 8;
  std::atomic<unsigned> started = 0, finished = 0, destroyed = 0;
  auto core = TaskCore::create(limits);
  CHECK(core);
  auto group = core->openRoot();
  CHECK(group);
  Input first{20ms, &started, &finished, &destroyed}, second{1ms, &started, &finished, &destroyed};
  auto a = submit(*core, group, first);
  auto b = submit(*core, group, second);
  CHECK(core->drain());
  CHECK(started == 2 && finished == 2 && core->settled(a) && core->settled(b));
  CHECK(core->failure(group).kind == TaskFailure::None && core->stats().timers == 0);
  Invocation destination;
  std::uint64_t result = 0;
  CHECK(core->observe(a, layout, destination, &result) && result == 42);
  CHECK(core->observe(b, layout, destination, &result) && result == 42);
  CHECK(core->close(group) && core->drain());
  CHECK(core->release(group));
  CHECK(destroyed == 2 && !core->stats().retainedBytes);
  // Reuse the slots after cancellation. No stale deadline may resume the new frame.
  for (bool deadline : {false, true}) {
    group = core->openRoot();
    CHECK(group);
    Input longSleep{std::chrono::hours(24), &started, &finished, &destroyed};
    auto task = submit(*core, group, longSleep);
    CHECK(core->close(group));
    const auto count = started.load() + 1;
    std::thread controller([&] { CHECK(core->drain()); });
    awaitStarted(started, count);
    // Wait for the execution lease to publish its timer before checking its bound.
    const auto limit = Clock::now() + 10s;
    while (!core->stats().timers && Clock::now() < limit)
      std::this_thread::yield();
    CHECK(core->stats().timers == 1 && core->stats().records == 1);
    const auto before = Clock::now();
    CHECK(core->cancel(group, deadline));
    controller.join();
    CHECK(Clock::now() - before < 5s);
    CHECK(core->settled(task) && core->stats().timers == 0);
    CHECK(core->failure(group).kind ==
          (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled));
    CHECK(core->release(group));
    group = core->openRoot();
    CHECK(group);
    Input zero{0ns, &started, &finished, &destroyed};
    auto recycled = submit(*core, group, zero);
    CHECK(recycled.generation != task.generation);
    CHECK(core->drain());
    CHECK(core->observe(recycled, layout, destination, &result));
    CHECK(core->close(group) && core->drain());
    CHECK(core->failure(group).kind == TaskFailure::None && core->stats().timers == 0);
    CHECK(core->release(group));
  }
  // A newly submitted shorter timer wakes a controller waiting on a long one.
  // This also proves progress with a single worker: the long sleeper cannot own it.
  group = core->openRoot();
  Input longWait{std::chrono::hours(24), &started, &finished, &destroyed};
  submit(*core, group, longWait);
  const auto waiting = started.load() + 1;
  std::thread controller([&] { CHECK(core->drain()); });
  awaitStarted(started, waiting);
  const auto limit = Clock::now() + 10s;
  while (!core->stats().timers && Clock::now() < limit)
    std::this_thread::yield();
  CHECK(core->stats().timers == 1);
  const auto completes = finished.load() + 1;
  Input shortWait{1ms, &started, &finished, &destroyed};
  auto shorter = submit(*core, group, shortWait);
  awaitStarted(finished, completes);
  while (!core->settled(shorter) && Clock::now() < limit)
    std::this_thread::yield();
  CHECK(core->settled(shorter) && core->observe(shorter, layout, destination, &result));
  CHECK(core->close(group) && core->cancel(group));
  controller.join();
  CHECK(core->stats().timers == 0 && core->release(group));
  // Timer registration/expiry require no scheduler allocation after task admission.
  group = core->openRoot();
  submit(*core, group, longWait);
  core->failAllocationAt(1);
  CHECK(core->step() && core->stats().timers == 1);
  CHECK(core->cancel(group) && core->close(group) && core->drain());
  CHECK(core->release(group));
  core->failAllocationAt(0);
  // Source faults retain their kind and position after a suspension.
  group = core->openRoot();
  Input bad{1ms, &started, &finished, &destroyed, true};
  submit(*core, group, bad);
  CHECK(core->close(group) && core->drain());
  auto failure = core->failure(group);
  CHECK(failure.kind == TaskFailure::Fault && failure.code == 1 && failure.line == 17);
  CHECK(core->release(group));
  CHECK(!core->stats().timers && !core->stats().retainedBytes && !core->stats().unfinished);
  // Destruction cancels pending timers, even without a controller/drain call.
  group = core->openRoot();
  Input pending{std::chrono::hours(24), &started, &finished, &destroyed};
  submit(*core, group, pending);
  CHECK(core->step());
  CHECK(core->stats().timers == 1);
  const auto before = Clock::now();
  core.reset();
  CHECK(Clock::now() - before < 5s);
}
int main() {
  Invocation ctx;
  CHECK(time(ctx, TimeAction::Nanoseconds, UINT64_MAX).value == UINT64_MAX);
  constexpr auto maxMillis = UINT64_MAX / 1'000'000;
  CHECK(time(ctx, TimeAction::Milliseconds, maxMillis).value == maxMillis * 1'000'000);
  CHECK(time(ctx, TimeAction::Milliseconds, maxMillis + 1).kind == TimeKind::OutOfRange);
  CHECK(time(ctx, TimeAction::AsNanoseconds, UINT64_MAX).value == UINT64_MAX);
  CHECK(std::bit_cast<double>(time(ctx, TimeAction::AsSeconds, 1'500'000'000).value) == 1.5);
  auto start = time(ctx, TimeAction::Now);
  CHECK(start.kind == TimeKind::Success);
  auto elapsed = time(ctx, TimeAction::Elapsed, start.value);
  CHECK(elapsed.kind == TimeKind::Success);
  CHECK(time(ctx, TimeAction::Elapsed, UINT64_MAX).kind == TimeKind::OutOfRange);
  const auto huge = time(ctx, TimeAction::Sleep, UINT64_MAX);
  CHECK(huge.kind == TimeKind::Success || huge.kind == TimeKind::OutOfRange);
  CHECK(ctx.status() == 0 && !ctx.live());
  Invocation stopped;
  std::atomic<bool> cancelled = true;
  stopped.bindCancellation(&cancelled);
  CHECK(time(stopped, TimeAction::Now).value == 0 && stopped.poll() && !stopped.status());
  stopped.fault(1);
  CHECK(time(stopped, TimeAction::Now).value == 0 && stopped.status() == 1);
  Invocation invalid;
  TimeResult result;
  kalandra_time_v1(&invalid, &result, 999, 0);
  CHECK(invalid.status() == 3 && !result.value);
  for (unsigned workers : {1u, 4u}) {
    timerCases(workers);
    std::atomic<unsigned> started = 0, finished = 0, destroyed = 0;
    TaskLimits limits;
    limits.workers = workers;
    auto runner = ClosedTaskRunner::create(limits);
    CHECK(runner);
    Input input{1ms, &started, &finished, &destroyed};
    Invocation destination;
    std::uint64_t output = 0;
    CHECK(runner->run(entry, {72, &input, sizeof(input)}, layout, destination, &output).kind ==
          TaskFailure::None);
    CHECK(output == 42 && started == 1 && finished == 1 && destroyed == 1 &&
          runner->stats().timers == 0);
  }
  std::puts("PASS: monotonic time, checked conversions, timer progress, cancellation, fault "
            "precedence and cleanup");
}
