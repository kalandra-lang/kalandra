#define main generated_main
#include "host.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Mixed source assertion %d: %s\n", __LINE__, #x);                       \
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
    for (std::uint64_t fail = 1; fail <= 64; ++fail) {
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
        CHECK(std::bit_cast<std::uint32_t>(first) == 0x42300000u && first == second);
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
  }
  // Cancel while GPU work retains the source frame, before the following CPU calls.
  failureAttempt = 0;
  runtime->holdCompletions(true);
  auto core = TaskCore::create({});
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
  CHECK(core->cancel(group));
  runtime->holdCompletions(false);
  CHECK(core->drain());
  CHECK(core->failure(task).kind == TaskFailure::Cancelled);
  clean(*core, group, *runtime);
  std::puts("Mixed source CPU/GPU result, allocation failures, cached observation and cancellation "
            "passed");
}
