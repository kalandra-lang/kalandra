#define main generated_main
#include "host.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Host expression assertion %d: %s\n", __LINE__, #x);                    \
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
        !s.stagingBuffers);
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
        CHECK(std::bit_cast<std::uint32_t>(first) == 0x432d0000u && first == second);
        reachedSuccess = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        ++faults;
      }
      clean(*core, group, *runtime);
      if (reachedSuccess)
        break;
    }
    CHECK(reachedSuccess && faults >= 5);
    std::printf("%u workers: %u inline CPU allocation failures unwound\n", workers, faults);
  }
  failureAttempt = 0;
  auto run = [&](bool success) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    CHECK((core->failure(task).kind == TaskFailure::None) == success);
    clean(*core, group, *runtime);
  };
  runtime->failAllocationAt(0);
  run(true);
  const auto attempts = runtime->stats().attempts;
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    runtime->failAllocationAt(fail);
    run(false);
  }
  runtime->failAllocationAt(0);
  std::printf("%llu Metal preparation failures unwound\n",
              static_cast<unsigned long long>(attempts));
  for (unsigned scenario : {0u, 1u, 2u}) {
    runtime->holdCompletions(true);
    if (scenario == 2)
      runtime->failNextCompletion();
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task);
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!core->stats().externalPending) {
      core->step();
      CHECK(std::chrono::steady_clock::now() < end);
      std::this_thread::yield();
    }
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    runtime->holdCompletions(false);
    CHECK(core->drain());
    CHECK(core->failure(task).kind != TaskFailure::None);
    CHECK(core->failure(group).kind == (scenario == 0   ? TaskFailure::Cancelled
                                        : scenario == 1 ? TaskFailure::DeadlineExceeded
                                                        : TaskFailure::Fault));
    clean(*core, group, *runtime);
  }
  auto limited = m::Runtime::create({.maxBytes = 16});
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
  std::puts("Inline host CPU/GPU result, allocation failures, cached observation, budgets, "
            "cancellation, deadline and failed completion passed");
}
