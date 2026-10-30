#define main generated_main
#include "host.cpp"
#undef main
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Spawned helper assertion %d: %s\n", __LINE__, #x);                     \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
void empty(m::Runtime &runtime) {
  auto s = runtime.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.inFlight &&
        !s.stagingBuffers && !s.argumentBytes && !s.argumentPacks);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
}
m::Buffer input(m::Runtime &runtime, std::int64_t count = 3) {
  Invocation context;
  auto *data = static_cast<float *>(context.allocate(count * sizeof(float)));
  CHECK(data);
  for (std::int64_t i = 0; i < count; ++i)
    data[i] = float(i + 1);
  Float32Tensor<1> descriptor{data, data, 0, {count}, {1}};
  auto value = runtime.fromCpu(context, descriptor.view());
  CHECK(value);
  context.release(data);
  return *value;
}
m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
void verify(const m::Buffer &value) {
  auto bytes = value.hostBytes();
  std::array<std::uint32_t, 3> expected{0x40800000u, 0x41000000u, 0x41400000u};
  CHECK(bytes && bytes->size() == sizeof(expected) &&
        !std::memcmp(bytes->data(), expected.data(), sizeof(expected)));
}
template <class F> void until(TaskCore &core, F condition) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition()) {
    core.step();
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
std::uint64_t failureAttempt = 0;
bool injectedChild(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(failureAttempt);
  return helper2::construct(context, frame, packet);
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  for (unsigned workers : {1u, 2u, 4u}) {
    TaskLimits limits;
    limits.workers = workers;
    // Packets and their submitting Invocation/directory may disappear immediately.
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    TaskHandle task;
    {
      auto values = input(*runtime);
      std::string directory = argv[1];
      helper2::PacketData packet{runtime.get(), directory.c_str(), values, values, 2};
      task = core->submit(group, helper2::entry,
                          {helper2::entry.packetLayout, &packet, sizeof(packet)});
      packet = {};
    }
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::None);
    auto first = observe(*core, task), second = observe(*core, task);
    retire(*core, group);
    core.reset();
    verify(first);
    verify(second);
    CHECK(first.hostBytes()->data() == second.hostBytes()->data());
    first = {};
    second = {};
    empty(*runtime);

    bool success = false;
    unsigned failures = 0;
    for (failureAttempt = 1; failureAttempt <= 128; ++failureAttempt) {
      core = TaskCore::create(limits);
      CHECK(core);
      group = core->openRoot();
      auto injected = helper2::entry;
      injected.construct = injectedChild;
      {
        auto values = input(*runtime);
        helper2::PacketData packet{runtime.get(), argv[1], values, values, 2};
        task = core->submit(group, injected, {injected.packetLayout, &packet, sizeof(packet)});
      }
      CHECK(task && core->drain());
      auto failure = core->failure(task);
      if (failure.kind == TaskFailure::None) {
        verify(observe(*core, task));
        success = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        ++failures;
      }
      retire(*core, group);
      empty(*runtime);
      if (success)
        break;
    }
    CHECK(success && failures);
    failureAttempt = 0;
    // Parent cancellation reaches suspended grandchildren and GPU completions.
    for (unsigned scenario : {0u, 1u, 2u}) {
      core = TaskCore::create(limits);
      CHECK(core);
      group = core->openRoot();
      runtime->holdCompletions(true);
      PacketData packet{runtime.get(), argv[1]};
      task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
      CHECK(task);
      until(*core, [&] { return core->stats().externalPending >= 2; });
      if (scenario < 2)
        CHECK(core->cancel(group, scenario == 1));
      else
        runtime->failNextCompletion();
      runtime->holdCompletions(false);
      CHECK(core->drain());
      auto failure = core->failure(task);
      CHECK(failure.kind == (scenario < 2 ? TaskFailure::Cancelled : TaskFailure::Fault));
      CHECK(core->failure(group).kind == (scenario == 0   ? TaskFailure::Cancelled
                                          : scenario == 1 ? TaskFailure::DeadlineExceeded
                                                          : TaskFailure::Fault));
      retire(*core, group);
      empty(*runtime);
    }
    // Cancelling an independent invocation does not poison another helper.
    core = TaskCore::create(limits);
    CHECK(core);
    auto a = core->openRoot(), b = core->openRoot();
    runtime->holdCompletions(true);
    TaskHandle left, right;
    {
      auto values = input(*runtime);
      helper2::PacketData packet{runtime.get(), argv[1], values, values, 2};
      left =
          core->submit(a, helper2::entry, {helper2::entry.packetLayout, &packet, sizeof(packet)});
      right =
          core->submit(b, helper2::entry, {helper2::entry.packetLayout, &packet, sizeof(packet)});
    }
    CHECK(left && right);
    until(*core, [&] { return core->stats().externalPending >= 2; });
    CHECK(core->cancel(a));
    runtime->holdCompletions(false);
    CHECK(core->drain() && core->failure(left).kind == TaskFailure::Cancelled &&
          core->failure(right).kind == TaskFailure::None);
    verify(observe(*core, right));
    CHECK(core->close(a) && core->close(b) && core->release(a));
    retire(*core, b);
    empty(*runtime);
  }
  // Unknown fixed-parameter constraints fault in the child before arithmetic.
  {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    TaskHandle task;
    {
      auto values = input(*runtime, 2);
      helper2::PacketData packet{runtime.get(), argv[1], values, values, 2};
      task = core->submit(group, helper2::entry,
                          {helper2::entry.packetLayout, &packet, sizeof(packet)});
    }
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::Fault &&
          core->failure(task).code == 1);
    retire(*core, group);
    empty(*runtime);
  }
  // Sweep task-core frame/result allocation failures through two spawn levels.
  bool success = false;
  unsigned failures = 0;
  for (unsigned failure = 1; failure <= 128; ++failure) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    core->failAllocationAt(failure);
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    if (core->failure(task).kind == TaskFailure::None)
      success = true;
    else {
      CHECK(core->failure(task).kind == TaskFailure::Fault);
      ++failures;
    }
    retire(*core, group);
    empty(*runtime);
    if (success)
      break;
  }
  CHECK(success && failures > 2);
  auto run = [&] {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    auto failure = core->failure(task);
    CHECK(failure.kind == TaskFailure::None || failure.kind == TaskFailure::Fault);
    retire(*core, group);
    empty(*runtime);
    return failure;
  };
  runtime->failAllocationAt(0);
  CHECK(run().kind == TaskFailure::None);
  auto attempts = runtime->stats().attempts;
  for (std::uint64_t failure = 1; failure <= attempts; ++failure) {
    runtime->failAllocationAt(failure);
    CHECK(run().kind == TaskFailure::Fault);
  }
  runtime->failAllocationAt(0);
  for (unsigned scenario : {0u, 1u, 2u}) {
    TaskLimits limits;
    if (scenario == 0)
      limits.depth = 1;
    else if (scenario == 1)
      limits.records = 1;
    else
      limits.bytes = entry.frameBytes + entry.result->bytes;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    const auto failure = core->failure(task);
    if (scenario == 2) {
      // The task itself fits. Its first CPU-owned tensor allocation exhausts
      // the byte budget during execution, which is an allocation fault.
      CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
    } else {
      CHECK(failure.kind == TaskFailure::AdmissionRejected);
    }
    retire(*core, group);
    empty(*runtime);
  }
  std::puts("Spawned helper captures/results, bounded nested scheduling, allocation failures, "
            "held completions, cancellation/deadlines and independent invocation isolation passed");
}
