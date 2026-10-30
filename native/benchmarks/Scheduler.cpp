#include "../runtime/Tasks.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace kalandra::runtime;
using Clock = std::chrono::steady_clock;
static void check(bool ok) {
  if (!ok) {
    std::fputs("Scheduler benchmark validation failed\n", stderr);
    std::exit(1);
  }
}
struct Frame {
  unsigned remaining;
};
static bool construct(Invocation &, void *out, const Packet &input) {
  new (out) Frame{*static_cast<const unsigned *>(input.data)};
  return true;
}
static bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<float *>(out) = *static_cast<const float *>(in);
  return true;
}
static void nothing(void *) noexcept {}
static const ValueLayout scalar{1, sizeof(float), alignof(float), copy, nothing};
static void resume(Task &task) {
  auto &frame = *static_cast<Frame *>(task.frame());
  if (--frame.remaining)
    task.yield();
  else {
    const float result = 1;
    task.complete(&result);
  }
}
static const TaskEntry entry{
    1,       2,      sizeof(unsigned), alignof(unsigned), sizeof(Frame), alignof(Frame), construct,
    nothing, resume, &scalar};
static std::array<double, 5> batch(TaskCore &core, unsigned hops) {
  std::array<Clock::time_point, 6> time;
  time[0] = Clock::now();
#ifdef KALANDRA_TASK_PROFILE
  const auto beforeAdmission = core.profile().notifications;
#endif
  auto group = core.openRoot();
  std::array<TaskHandle, 16> jobs;
  for (auto &job : jobs)
    job = core.submit(group, entry, {2, &hops, sizeof(hops)});
  time[1] = Clock::now();
#ifdef KALANDRA_TASK_PROFILE
  check(core.profile().notifications == beforeAdmission);
#endif
  check(core.drain());
  time[2] = Clock::now();
#ifdef KALANDRA_TASK_PROFILE
  const auto afterExecution = core.profile().notifications;
#endif
  for (auto job : jobs) {
    Invocation reader;
    float result = 0;
    check(core.observe(job, scalar, reader, &result) && result == 1 && !reader.live());
  }
  time[3] = Clock::now();
  check(core.close(group) && core.drain() && core.failure(group).kind == TaskFailure::None);
  time[4] = Clock::now();
  check(core.release(group));
  time[5] = Clock::now();
#ifdef KALANDRA_TASK_PROFILE
  check(core.profile().notifications == afterExecution);
#endif
  auto stats = core.stats();
  check(!stats.records && !stats.groups && !stats.unfinished && !stats.retainedOwners &&
        !stats.retainedBytes && !stats.ready && !stats.pending && !stats.maintenanceTasks);
  std::array<double, 5> elapsed;
  for (unsigned i = 0; i < elapsed.size(); ++i)
    elapsed[i] = std::chrono::duration<double, std::micro>(time[i + 1] - time[i]).count();
  return elapsed;
}
int main(int argc, char **argv) {
  if (argc != 5)
    return 2;
  TaskLimits limits;
  limits.workers = std::strtoul(argv[1], nullptr, 10);
  limits.records = std::strtoul(argv[2], nullptr, 10);
  limits.groups = std::strtoul(argv[3], nullptr, 10);
  const unsigned hops = std::strtoul(argv[4], nullptr, 10);
  check(hops && limits.records >= 16);
  auto core = TaskCore::create(limits);
  check(bool(core));
#ifdef KALANDRA_TASK_PROFILE
  // Fresh-core diagnostic: creation/initial worker waits are included. No
  // profiling clocks/counters are used for the separate latency samples.
  check(core->drain(0));
  check(core->profile().notifications == 0 && core->profile().recordVisits == 0 &&
        core->profile().groupVisits == 0);
  batch(*core, hops);
  auto s = core->stats();
  auto p = core->profile();
  check(s.resumes == 16 * hops && s.publications == 16 && p.allocationAttempts == 32 &&
        p.frees == 32 && p.steps >= s.resumes && p.recordVisits && !p.readyVisits &&
        p.readyPushes == s.resumes && p.runnableEvents == s.resumes);
  // Task scans are needed only while a waiter or settlement is present.
  check(p.taskPasses + p.idleTaskPasses == p.maintenancePasses && p.idleTaskPasses &&
        p.recordVisits <= 16 * p.taskPasses && p.groupVisits <= p.maintenancePasses);
  check(p.notifications == p.workerSignals + p.workerBroadcasts + p.controllerSignals);
  if (limits.workers > 1)
    check(p.workerBroadcasts == 1 && p.controllerSignals && p.controllerWaitReturns);
  else
    check(!p.notifications && !p.workerWaitReturns && !p.controllerWaitReturns);
  std::printf("{\"locks\":%llu,\"contended_locks\":%llu,\"lock_wait_ns\":%llu,"
              "\"notifications\":%llu,\"waits\":%llu,\"wait_returns\":%llu,\"wait_ns\":%llu,"
              "\"allocation_attempts\":%llu,\"frees\":%llu,\"allocator_ns\":%llu,"
              "\"maintenance_passes\":%llu,\"task_passes\":%llu,\"idle_task_passes\":%llu,"
              "\"record_visits\":%llu,\"group_visits\":%llu,"
              "\"ready_visits\":%llu,\"steps\":%llu,\"resumes\":%llu,"
              "\"worker_signals\":%llu,\"worker_broadcasts\":%llu,\"controller_signals\":%llu,"
              "\"worker_wait_returns\":%llu,\"controller_wait_returns\":%llu,"
              "\"runnable_events\":%llu,\"ready_pushes\":%llu,\"pending_promotions\":%llu,\"peak_"
              "pending\":%llu}\n",
              (unsigned long long)p.locks, (unsigned long long)p.contendedLocks,
              (unsigned long long)p.lockWaitNs, (unsigned long long)p.notifications,
              (unsigned long long)p.waits, (unsigned long long)p.waitReturns,
              (unsigned long long)p.waitNs, (unsigned long long)p.allocationAttempts,
              (unsigned long long)p.frees, (unsigned long long)p.allocatorNs,
              (unsigned long long)p.maintenancePasses, (unsigned long long)p.taskPasses,
              (unsigned long long)p.idleTaskPasses, (unsigned long long)p.recordVisits,
              (unsigned long long)p.groupVisits, (unsigned long long)p.readyVisits,
              (unsigned long long)p.steps, (unsigned long long)s.resumes,
              (unsigned long long)p.workerSignals, (unsigned long long)p.workerBroadcasts,
              (unsigned long long)p.controllerSignals, (unsigned long long)p.workerWaitReturns,
              (unsigned long long)p.controllerWaitReturns, (unsigned long long)p.runnableEvents,
              (unsigned long long)p.readyPushes, (unsigned long long)p.pendingPromotions,
              (unsigned long long)p.peakPending);
#else
  for (unsigned i = 0; i < 10; ++i)
    batch(*core, hops);
  for (unsigned round = 0; round < 9; ++round) {
    std::array<double, 5> sums{};
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
      auto sample = batch(*core, hops);
      for (unsigned i = 0; i < sums.size(); ++i)
        sums[i] += sample[i] / 10;
    }
    std::printf("{\"round\":%u,\"admit_us\":%.6f,\"execute_us\":%.6f,\"observe_us\":%.6f,"
                "\"close_us\":%.6f,\"release_us\":%.6f}\n",
                round, sums[0], sums[1], sums[2], sums[3], sums[4]);
  }
#endif
}
