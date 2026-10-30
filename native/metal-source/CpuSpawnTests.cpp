#define main generated_main
#include "host.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "CPU spawn assertion %d: %s\n", __LINE__, #x);                          \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
std::uint64_t failureAttempt = 0;
bool injectedConstruct(Invocation &context, void *frame, const Packet &packet) {
  context.failAllocationAt(failureAttempt);
  return construct(context, frame, packet);
}
void empty(m::Runtime &runtime) {
  auto s = runtime.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.inFlight &&
        !s.stagingBuffers && !s.argumentBytes && !s.argumentPacks);
}
void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group));
  CHECK(core.drain());
  CHECK(core.release(group));
  auto s = core.stats();
  CHECK(!s.records && !s.groups && !s.retainedBytes && !s.retainedOwners && !s.externalPending);
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
  CHECK(!value.onDevice() && value.shape().size() == 2 && value.shape()[0] == 2 &&
        value.shape()[1] == 2);
  auto bytes = value.hostBytes();
  CHECK(bytes && bytes->size() == 16);
  std::array<std::uint32_t, 4> bits{};
  std::memcpy(bits.data(), bytes->data(), 16);
  CHECK((bits == std::array<std::uint32_t, 4>{0x42500000u, 0x42580000u, 0x42800000u, 0x42840000u}));
}
template <class F> void until(TaskCore &core, F condition) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!condition()) {
    core.step();
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
m::Buffer matrix(m::Runtime &runtime, std::int64_t rows, std::int64_t columns,
                 std::initializer_list<float> values) {
  Invocation context;
  CHECK(std::size_t(rows * columns) == values.size());
  auto *data = static_cast<float *>(context.allocate(values.size() * sizeof(float)));
  CHECK(data);
  if (values.size())
    std::memcpy(data, values.begin(), values.size() * sizeof(float));
  Float32Tensor<2> descriptor{data, data, 0, {rows, columns}, {columns, 1}};
  auto value = runtime.fromCpu(context, descriptor.view());
  CHECK(value);
  context.release(data);
  return *value;
}
void bits(const m::Buffer &value, std::initializer_list<std::uint32_t> expected) {
  auto bytes = value.hostBytes();
  CHECK(bytes && bytes->size() == expected.size() * 4);
  CHECK(bytes->empty() || !std::memcmp(bytes->data(), expected.begin(), bytes->size()));
}
std::uint64_t childFailure = 0;
bool (*childConstructor)(Invocation &, void *, const Packet &) = nullptr;
bool injectChild(Invocation &context, void *out, const Packet &packet) {
  context.failAllocationAt(childFailure);
  return childConstructor(context, out, packet);
}
std::atomic<unsigned> entered{0};
std::atomic<bool> proceed{false};
void gatedCpu(Task &task) {
  entered.fetch_add(1, std::memory_order_release);
  while (!proceed.load(std::memory_order_acquire))
    std::this_thread::yield();
  cpuJob0Resume(task);
}
void cpuEntries(m::Runtime &runtime) {
  // Entry construction must retain captures before the submitting packet and
  // its invocation disappear. Both arguments may alias one immutable backing.
  for (unsigned scenario : {0u, 1u}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    TaskHandle task;
    {
      auto input = matrix(runtime, 2, 2, {1, 0, 0, 1});
      if (scenario == 0) {
        cpuJob0Packet packet{&runtime, input, input};
        task =
            core->submit(group, cpuJob0Entry, {cpuJob0Entry.packetLayout, &packet, sizeof(packet)});
        packet = {};
      } else {
        cpuJob2Packet packet{&runtime, input, 3};
        task =
            core->submit(group, cpuJob2Entry, {cpuJob2Entry.packetLayout, &packet, sizeof(packet)});
        packet = {};
      }
    }
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::None);
    auto first = observe(*core, task), second = observe(*core, task);
    retire(*core, group);
    core.reset();
    CHECK(first.hostBytes()->data() == second.hostBytes()->data());
    if (scenario == 0)
      bits(first, {0x3f800000u, 0, 0, 0x3f800000u});
    else
      bits(first, {0x40800000u, 0x40400000u, 0x40400000u, 0x40800000u});
    first = {};
    second = {};
    empty(runtime);
  }
  auto input = matrix(runtime, 2, 2, {1, 0, 0, 1});
  auto fixed = matrix(runtime, 2, 3, {1, 2, 3, 4, 5, 6});
  auto sweep = [&](const TaskEntry &prototype, const auto &packet, std::uint32_t expected) {
    auto baseline = runtime.stats();
    for (unsigned workers : {1u, 2u, 4u}) {
      bool success = false;
      unsigned failures = 0;
      for (childFailure = 1; childFailure <= 128; ++childFailure) {
        auto injected = prototype;
        childConstructor = prototype.construct;
        injected.construct = injectChild;
        TaskLimits limits;
        limits.workers = workers;
        auto core = TaskCore::create(limits);
        CHECK(core);
        auto group = core->openRoot();
        auto task = core->submit(group, injected, {injected.packetLayout, &packet, sizeof(packet)});
        CHECK(task && core->drain());
        auto failure = core->failure(task);
        if (failure.kind == TaskFailure::None) {
          if (expected) {
            Invocation context;
            float result = 0;
            CHECK(core->observe(task, m::Runtime::scalarLayout(), context, &result));
            CHECK(std::bit_cast<std::uint32_t>(result) == expected);
          } else {
            bits(observe(*core, task), {0x3f800000u, 0, 0, 0x3f800000u});
          }
          success = true;
        } else {
          CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
          ++failures;
        }
        retire(*core, group);
        CHECK(runtime.stats().bytes == baseline.bytes &&
              runtime.stats().hostBackings == baseline.hostBackings);
        if (success)
          break;
      }
      CHECK(success && failures > 0);
      std::printf("%u workers: %u child CPU allocation failures unwound\n", workers, failures);
    }
    childFailure = 0;
  };
  cpuJob0Packet project{&runtime, input, input};
  cpuJob1Packet total{&runtime, input};
  cpuJob3Packet fixedTotal{&runtime, fixed};
  sweep(cpuJob0Entry, project, 0);
  sweep(cpuJob1Entry, total, 0x40000000u);
  sweep(cpuJob3Entry, fixedTotal, 0x41a80000u);
  // Task-core frame/result admission failures release every retained capture.
  for (std::uint64_t failure = 1; failure <= 2; ++failure) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    core->failAllocationAt(failure);
    auto task =
        core->submit(group, cpuJob0Entry, {cpuJob0Entry.packetLayout, &project, sizeof(project)});
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::Fault);
    retire(*core, group);
  }
  // A worker must not run an invalid fixed-shape invocation or publish its output.
  {
    cpuJob3Packet bad{&runtime, input};
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    auto task = core->submit(group, cpuJob3Entry, {cpuJob3Entry.packetLayout, &bad, sizeof(bad)});
    CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::Fault &&
          core->failure(task).code == 1);
    retire(*core, group);
  }
  // Two actual compiled CPU entries run concurrently. Cancelling one independent
  // group does not poison the other group's result or borrowed input captures.
  entered = 0;
  proceed = false;
  {
    TaskLimits limits;
    limits.workers = 2;
    auto core = TaskCore::create(limits);
    CHECK(core);
    auto a = core->openRoot(), b = core->openRoot();
    auto gated = cpuJob0Entry;
    gated.resume = gatedCpu;
    auto first = core->submit(a, gated, {gated.packetLayout, &project, sizeof(project)});
    auto second = core->submit(b, gated, {gated.packetLayout, &project, sizeof(project)});
    CHECK(first && second);
    std::thread controller([&] { CHECK(core->drain()); });
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (entered.load(std::memory_order_acquire) != 2) {
      CHECK(std::chrono::steady_clock::now() < end);
      std::this_thread::yield();
    }
    CHECK(core->stats().peakRunning == 2 && core->cancel(a));
    proceed.store(true, std::memory_order_release);
    controller.join();
    CHECK(core->failure(first).kind == TaskFailure::Cancelled &&
          core->failure(second).kind == TaskFailure::None);
    bits(observe(*core, second), {0x3f800000u, 0, 0, 0x3f800000u});
    CHECK(core->close(a) && core->close(b) && core->drain());
    CHECK(core->release(a) && core->release(b));
    CHECK(!core->stats().retainedBytes && !core->stats().retainedOwners);
  }
  project = {};
  total = {};
  fixedTotal = {};
  input = {};
  fixed = {};
  empty(runtime);
  std::puts("CPU child capture ownership, allocation sweeps, concurrent execution and cancellation "
            "isolation passed");
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && generated_main(argc, argv) == 0);
  auto made = m::Runtime::create();
  CHECK(made);
  auto runtime = std::move(*made);
  cpuEntries(*runtime);
  for (unsigned workers : {1u, 2u, 4u}) {
    bool success = false;
    unsigned failures = 0;
    for (std::uint64_t fail = 1; fail <= 128; ++fail) {
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
        auto first = observe(*core, task), second = observe(*core, task);
        CHECK(first.hostBytes()->data() == second.hostBytes()->data());
        // Returned CPU backing survives the invocation, scheduler and all GPU intermediates.
        retire(*core, group);
        core.reset();
        verify(first);
        verify(second);
        first = {};
        second = {};
        success = true;
      } else {
        CHECK(failure.kind == TaskFailure::Fault && failure.code == 2);
        retire(*core, group);
        ++failures;
      }
      empty(*runtime);
      if (success)
        break;
    }
    CHECK(success && failures >= 4);
    std::printf("%u workers: %u CPU allocation failures unwound\n", workers, failures);
  }
  failureAttempt = 0;
  // Include CPU-result lease construction in the Metal allocation failure sweep.
  runtime->failAllocationAt(0);
  auto run = [&](bool succeeds) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task && core->drain());
    if (succeeds) {
      CHECK(core->failure(task).kind == TaskFailure::None);
      verify(observe(*core, task));
    } else
      CHECK(core->failure(task).kind != TaskFailure::None);
    retire(*core, group);
    empty(*runtime);
  };
  run(true);
  auto attempts = runtime->stats().attempts;
  CHECK(attempts > 0);
  for (std::uint64_t fail = 1; fail <= attempts; ++fail) {
    runtime->failAllocationAt(fail);
    run(false);
  }
  runtime->failAllocationAt(0);
  for (unsigned scenario : {0u, 1u, 2u}) {
    auto core = TaskCore::create();
    CHECK(core);
    auto group = core->openRoot();
    PacketData packet{runtime.get(), argv[1]};
    auto before = runtime->stats().completions, submissions = runtime->stats().submissions;
    auto task = core->submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
    CHECK(task);
    constexpr unsigned uploads = 1;
    until(*core, [&] { return runtime->stats().completions >= before + uploads; });
    runtime->holdCompletions(true);
    if (scenario == 2)
      runtime->failNextCompletion();
    until(*core, [&] {
      return runtime->stats().submissions == submissions + uploads + 1 &&
             core->stats().externalPending == 1;
    });
    if (scenario < 2)
      CHECK(core->cancel(group, scenario == 1));
    runtime->holdCompletions(false);
    CHECK(core->drain());
    CHECK(core->failure(task).kind != TaskFailure::None);
    retire(*core, group);
    empty(*runtime);
  }
  // The retained CPU results share the registry budget with transfers and GPU work.
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
  retire(*core, group);
  empty(**limited);
  std::puts("CPU task captures, tensor/scalar results, failure cleanup and mixed GPU cancellation "
            "passed");
}
