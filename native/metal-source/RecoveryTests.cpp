#define main generated_main
#include "host.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Recovery assertion %d: %s\n", __LINE__, #x);                           \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
std::uint64_t failureAttempt = 0;
bool injectedConstruct(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(failureAttempt);
  return construct(context, frame, packet);
}
void clean(TaskCore &core, GroupHandle group, m::Runtime &runtime) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.release(group));
  auto t = core.stats();
  auto s = runtime.stats();
  CHECK(!t.records && !t.groups && !t.retainedBytes && !t.retainedOwners && !t.externalPending);
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.inFlight &&
        !s.stagingBuffers && !s.argumentBytes && !s.argumentPacks);
}
unsigned scopeCancellation = 0;
bool scopeCancelled = false;
void controlledResume(Task &task) {
  auto &frame = *static_cast<Frame *>(task.frame());
  // Private generated continuation: the first GPU await in the protected scope.
  if (scopeCancellation && !scopeCancelled && frame.pc == gpuAwaitSites.front().pc) {
    CHECK(
        task.cancel(continuationGroup(frame, gpuAwaitSites.front().group), scopeCancellation == 2));
    scopeCancelled = true;
    // Let maintenance settle the failed scope before resuming admissions.
    task.yield();
    return;
  }
  resume(task);
}
std::uint32_t resultBits(TaskCore &core, TaskHandle task) {
  Invocation context;
  float result = 0;
  CHECK(core.observe(task, scalarLayout, context, &result));
  return std::bit_cast<std::uint32_t>(result);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  CHECK(generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  for (unsigned workers : {1u, 2u, 4u}) {
    bool reachedSuccess = false;
    unsigned faults = 0;
    for (std::uint64_t fail = 1; fail <= 256; ++fail) {
      failureAttempt = fail;
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{runtime.get(), argv[1]};
      auto injected = entry;
      injected.construct = injectedConstruct;
      auto task = core->submit(group, injected, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task && core->drain());
      auto failure = core->failure(task);
      if (failure.kind == TaskFailure::None) {
        Invocation context;
        float first = 0, second = 0;
        CHECK(core->observe(task, scalarLayout, context, &first));
        CHECK(core->observe(task, scalarLayout, context, &second));
        CHECK(std::bit_cast<std::uint32_t>(first) == 0x43340000u && first == second);
        reachedSuccess = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        ++faults;
      }
      clean(*core, group, *runtime);
      if (reachedSuccess)
        break;
    }
    CHECK(reachedSuccess && faults > 0);
    std::printf("%u workers: %u supervisor CPU allocation failures unwound\n", workers, faults);
  }
  failureAttempt = 0;
  unsigned recovered = 0, stopped = 0;
  auto run = [&] {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    if (core->failure(task).kind == TaskFailure::None) {
      auto bits = resultBits(*core, task);
      CHECK(bits == 0x43340000u || bits == 0x43700000u || bits == 0x43d20000u);
      ++recovered;
    } else {
      CHECK(core->failure(task).kind == TaskFailure::Fault);
      ++stopped;
    }
    clean(*core, group, *runtime);
  };
  runtime->failAllocationAt(0);
  run();
  const auto attempts = runtime->stats().attempts;
  recovered = 0;
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    runtime->failAllocationAt(fail);
    run();
  }
  runtime->failAllocationAt(0);
  CHECK(recovered && stopped && recovered + stopped == attempts);
  std::printf("%llu Metal preparation positions: %u recovered, %u stopped safely\n",
              static_cast<unsigned long long>(attempts), recovered, stopped);
  for (bool sharedUploads : {false, true}) {
    auto scenarioMade = m::Runtime::create({.sharedUploads = sharedUploads});
    CHECK(scenarioMade);
    auto runtime = std::move(*scenarioMade);
    auto target = runtime->device();
    CHECK(target);
    const auto computeOffset = sharedUploads && target->hasUnifiedMemory() ? 1u : 2u;
    for (unsigned scenario : {0u, 1u, 2u}) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{runtime.get(), argv[1]};
      auto submissions = runtime->stats().submissions;
      // Arm the protected compute before work runs. Shared uploads have no
      // command; private uploads contribute one blit. A completion count is
      // not a barrier for installing a gate after submission.
      runtime->holdCompletionAt(submissions + computeOffset);
      auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task);
      auto until = [&](auto condition) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!condition()) {
          core->step();
          CHECK(std::chrono::steady_clock::now() < end);
          std::this_thread::yield();
        }
      };
      // Any private upload remains free to finish; only the nested compute is held.
      until([&] {
        return runtime->stats().submissions == submissions + computeOffset &&
               core->stats().externalPending == 1;
      });
      if (scenario == 2)
        runtime->failNextCompletion();
      else
        CHECK(core->cancel(group, scenario == 1));
      runtime->holdCompletionAt(0);
      CHECK(core->drain());
      if (scenario == 2) {
        CHECK(core->failure(task).kind == TaskFailure::None);
        CHECK(resultBits(*core, task) == 0x43700000u);
      } else
        CHECK(core->failure(task).kind != TaskFailure::None);
      CHECK(core->failure(group).kind == (scenario == 0   ? TaskFailure::Cancelled
                                          : scenario == 1 ? TaskFailure::DeadlineExceeded
                                                          : TaskFailure::None));
      clean(*core, group, *runtime);
    }
  }
  for (unsigned workers : {1u, 2u, 4u})
    for (unsigned kind : {1u, 2u}) {
      scopeCancellation = kind;
      scopeCancelled = false;
      TaskLimits limits;
      limits.workers = workers;
      auto core = TaskCore::create(limits);
      CHECK(core);
      auto group = core->openRoot();
      PacketData packet{runtime.get(), argv[1]};
      auto controlled = entry;
      controlled.resume = controlledResume;
      auto task = core->submit(group, controlled, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task && core->drain() && scopeCancelled);
      CHECK(core->failure(task).kind == TaskFailure::None);
      CHECK(resultBits(*core, task) == (kind == 1 ? 0x43960000u : 0x43b40000u));
      clean(*core, group, *runtime);
    }
  scopeCancellation = 0;
  // A bounded child admission error is recoverable after the group settles.
  {
    TaskLimits limits;
    limits.records = 1;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    // The fallback's later GPU transfer also needs a record, so the owning
    // invocation ultimately propagates AdmissionRejected without conversion.
    CHECK(core->failure(task).kind == TaskFailure::AdmissionRejected);
    clean(*core, group, *runtime);
  }
  auto limited = m::Runtime::create({.maxBytes = 8});
  CHECK(limited);
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  PacketData packet{limited->get(), argv[1]};
  auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
  CHECK(task && core->drain());
  CHECK(core->failure(task).kind == TaskFailure::Fault &&
        core->failure(task).code == std::int64_t(m::Error::Budget));
  clean(*core, group, **limited);
  std::puts("Mixed TaskError matching, settled recovery, allocation failures, "
            "scope cancellation/deadline recovery and failed GPU completion recovery passed");
}
