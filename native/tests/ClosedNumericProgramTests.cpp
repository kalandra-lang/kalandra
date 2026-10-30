#include "../benchmarks/fair/TransformerBlock.h"
#include "../metal/Metal.h"
#include <atomic>
#include <new>
#include <thread>
using namespace kalandra::runtime;
namespace m = kalandra::runtime::metal;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "Closed GPU check %d: %s\n", __LINE__, #x);                             \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
#include "LegacyProgram.h"
#include "TransformerProgram.h"
static void retire(TaskCore &core, GroupHandle group) {
  CHECK(core.close(group) && core.drain() && core.release(group));
  auto s = core.stats();
  CHECK(!s.groups && !s.records && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
static void empty(const ClosedTaskRunner &runner) {
  auto s = runner.stats();
  CHECK(!s.groups && !s.records && !s.externalPending && !s.retainedBytes && !s.retainedOwners);
}
static m::Buffer observe(TaskCore &core, TaskHandle task) {
  Invocation context;
  alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
  CHECK(core.observe(task, m::Runtime::bufferLayout(), context, slot));
  auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
  auto result = *value;
  value->~Buffer();
  return result;
}
static std::vector<std::uint32_t> download(m::Runtime &runtime, m::Buffer buffer) {
  auto core = TaskCore::create();
  CHECK(core);
  auto group = core->openRoot();
  auto task = runtime.download(*core, group, buffer);
  CHECK(task && core->drain());
  auto value = observe(*core, task);
  auto bytes = value.hostBytes();
  CHECK(bytes);
  std::vector<std::uint32_t> result(bytes->size() / 4);
  std::memcpy(result.data(), bytes->data(), bytes->size());
  retire(*core, group);
  return result;
}
template <class F> static void waitFor(F ready) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!ready()) {
    CHECK(std::chrono::steady_clock::now() < end);
    std::this_thread::yield();
  }
}
struct Shared {
  std::atomic<bool> cancelled{false}, externalReady{false};
  ExternalCompletion external;
};
struct ParentInput {
  m::Runtime *runtime;
  Program *program;
  const std::array<m::Buffer, 7> *inputs;
  float scale;
  unsigned mode;
  Shared *shared;
};
struct Parent {
  ParentInput input;
  unsigned phase = 0;
  GroupHandle group{};
  TaskHandle gpu{}, other{};
  m::Buffer value{};
};
struct Sibling {
  Shared *shared;
  bool external;
  bool resumed = false;
};
static bool siblingConstruct(Invocation &, void *out, const Packet &packet) {
  new (out) Sibling(*static_cast<const Sibling *>(packet.data));
  return true;
}
static void nothing(void *) noexcept {}
static bool copyInt(Invocation &, void *out, const Invocation &, const void *in) noexcept {
  *static_cast<int *>(out) = *static_cast<const int *>(in);
  return true;
}
static const ValueLayout integer{0x43475001, sizeof(int), alignof(int), copyInt, nothing};
static void siblingResume(Task &task) {
  auto &f = *static_cast<Sibling *>(task.frame());
  if (f.external && !f.resumed) {
    f.resumed = true;
    f.shared->external = task.suspendExternal();
    CHECK(f.shared->external);
    f.shared->externalReady = true;
    return;
  }
  int value = 1;
  task.complete(&value);
}
static const TaskEntry sibling{1,
                               204,
                               sizeof(Sibling),
                               alignof(Sibling),
                               sizeof(Sibling),
                               alignof(Sibling),
                               siblingConstruct,
                               nothing,
                               siblingResume,
                               &integer};
static bool parentConstruct(Invocation &, void *out, const Packet &packet) {
  new (out) Parent{*static_cast<const ParentInput *>(packet.data)};
  return true;
}
static void parentDestroy(void *value) noexcept { static_cast<Parent *>(value)->~Parent(); }
static void parentResume(Task &task) {
  auto &f = *static_cast<Parent *>(task.frame());
  auto &in = f.input;
  if (!f.phase) {
    f.group = task.openGroup();
    CHECK(f.group);
    if (in.mode == 2) {
      Sibling packet{in.shared, true};
      f.other = task.spawn(f.group, sibling, {204, &packet, sizeof(packet)});
      CHECK(f.other);
    }
    const std::array<std::uint64_t, 3> captures{std::bit_cast<std::uint32_t>(transformer::epsilon),
                                                std::bit_cast<std::uint32_t>(in.scale),
                                                std::bit_cast<std::uint32_t>(transformer::epsilon)};
    f.gpu = in.runtime->executeNumericProgram(task, f.group, in.program->executable, *in.inputs,
                                              captures);
    CHECK(f.gpu);
    if (in.mode == 3) {
      Sibling packet{in.shared, false};
      f.other = task.spawn(f.group, sibling, {204, &packet, sizeof(packet)});
      CHECK(f.other);
    }
    f.phase = 1;
    if (in.mode == 1) {
      task.yield();
      return;
    }
  }
  if (f.phase == 1) {
    if (in.mode == 1) {
      CHECK(task.cancel(f.group, true));
      in.shared->cancelled = true;
    }
    if (!task.await(f.gpu))
      return;
    if (task.failure(f.gpu).kind == TaskFailure::None) {
      alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
      CHECK(task.read(f.gpu, m::Runtime::bufferLayout(), slot));
      auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
      f.value = *value;
      value->~Buffer();
    }
    f.phase = 2;
  }
  if (f.phase == 2) {
    if (f.other) {
      if (!task.await(f.other))
        return;
      int ignored = 0;
      CHECK(task.read(f.other, integer, &ignored) && ignored == 1);
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
                              205,
                              sizeof(ParentInput),
                              alignof(ParentInput),
                              sizeof(Parent),
                              alignof(Parent),
                              parentConstruct,
                              parentDestroy,
                              parentResume,
                              &m::Runtime::bufferLayout()};
static void test(const char *path, unsigned rows, unsigned width, bool enabled, bool full) {
  m::Options options;
  options.closedSynchronous = enabled;
  options.profile = true;
  auto made = m::Runtime::create(options);
  CHECK(made);
  auto &runtime = **made;
  {
    Program program(runtime, path);
    LegacyProgram legacy(runtime, path);
    transformer::Inputs data(rows, width, 1);
    std::array<m::Buffer, 7> inputs;
    for (unsigned i = 0; i < 7; ++i) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto host = runtime.hostTensor(std::as_bytes(std::span(data.values[i])), data.shapes[i],
                                     m::Format::Float32);
      CHECK(host);
      auto task = runtime.upload(*core, group, *host, *runtime.device());
      CHECK(task && core->drain());
      inputs[i] = observe(*core, task);
      retire(*core, group);
    }
    const auto baseline = runtime.stats();
    auto clean = [&] {
      auto s = runtime.stats();
      CHECK(
          s.bytes == baseline.bytes && s.metalBuffers == baseline.metalBuffers &&
          s.hostBackings == baseline.hostBackings && s.deviceBackings == baseline.deviceBackings &&
          s.argumentBytes == baseline.argumentBytes && s.argumentPacks == baseline.argumentPacks &&
          !s.inFlight && !s.stagingBuffers && !runtime.progressionCommandReservations());
    };
    auto reference = [&](float epsilon, float scale) {
      auto core = TaskCore::create();
      CHECK(core);
      auto group = core->openRoot();
      auto task = legacy.launch(runtime, *core, group, inputs, epsilon, scale);
      CHECK(task && core->drain() && core->failure(task).kind == TaskFailure::None);
      auto result = download(runtime, observe(*core, task));
      retire(*core, group);
      clean();
      return result;
    };
    std::vector<std::uint32_t> expected;
    for (unsigned workers : {1u, 4u}) {
      TaskLimits limits;
      limits.workers = workers;
      auto runner = ClosedTaskRunner::create(limits);
      CHECK(runner);
      for (unsigned i = 0; i < 8; ++i) {
        const auto epsilon = i % 2 ? .03f : transformer::epsilon;
        const auto scale = data.scale * (i + 1) / 4;
        auto target = reference(epsilon, scale);
        {
          const auto before = runtime.stats();
          auto result = program.launch(runtime, *runner, inputs, epsilon, scale);
          CHECK(result);
          empty(*runner);
          const auto after = runtime.stats();
          const auto bounded = rows >= 64 ? 5u : rows >= 16 ? 3u : 1u;
          CHECK(after.submissions - before.submissions == (enabled ? 1u : bounded));
          CHECK(after.dispatches - before.dispatches == (rows == 1 ? 11u : 12u));
          CHECK(download(runtime, *result) == target);
        }
        clean();
      }
      {
        auto first = program.launch(runtime, *runner, inputs, transformer::epsilon, data.scale);
        CHECK(first);
        expected = download(runtime, *first);
        auto second = program.launch(runtime, *runner, inputs, .03f, -data.scale);
        CHECK(second);
        CHECK(download(runtime, *first) ==
              expected); // Later invocations cannot overwrite an escaped result.
        runner.reset();
        CHECK(download(runtime, *first) == expected); // Result storage outlives its closed caller.
      }
      clean();
    }
    auto runner = ClosedTaskRunner::create();
    CHECK(runner);
    for (float invalid : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) {
      auto result = program.launch(runtime, *runner, inputs, invalid, data.scale);
      CHECK(!result && result.error().kind == TaskFailure::Fault && result.error().code == 4);
      empty(*runner);
      clean();
    }
    {
      auto result =
          runtime.executeNumericProgram(*runner, program.executable, inputs,
                                        std::array<std::uint64_t, 3>{UINT64_C(1) << 40, 0, 0});
      CHECK(!result && result.error().kind == TaskFailure::Fault && result.error().code == 3);
      result = runtime.executeNumericProgram(*runner, {}, inputs);
      CHECK(!result && result.error().kind == TaskFailure::Fault && result.error().code == 3);
      empty(*runner);
      clean();
    }
    if (full) {
      runtime.failAllocationAt(0);
      std::uint64_t attempts = 0;
      {
        auto result = program.launch(runtime, *runner, inputs, transformer::epsilon, data.scale);
        CHECK(result);
        attempts = runtime.stats().attempts;
      }
      clean();
      CHECK(attempts);
      for (std::uint64_t point = 1; point <= attempts; ++point) {
        runtime.failAllocationAt(point);
        auto result = program.launch(runtime, *runner, inputs, transformer::epsilon, data.scale);
        CHECK(!result && result.error().kind == TaskFailure::Fault && result.error().code == 2);
        runtime.failAllocationAt(0);
        empty(*runner);
        clean();
      }
      std::printf("closed=%d %ux%u: all %llu allocation checkpoints passed\n", enabled, rows, width,
                  (unsigned long long)attempts);
      // Already encoded work still acknowledges; its device failure retains
      // precedence over an allocation failure discovered later by the builder.
      runtime.failAllocationAt(attempts);
      runtime.failNextCompletion();
      const auto failedStart = runtime.stats().submissions;
      {
        auto result = program.launch(runtime, *runner, inputs, transformer::epsilon, data.scale);
        CHECK(!result && result.error().kind == TaskFailure::Fault &&
              result.error().code == std::int64_t(m::Error::Device));
      }
      CHECK(runtime.stats().submissions > failedStart);
      runtime.failAllocationAt(0);
      empty(*runner);
      clean();
      {
        // Missing work metadata and long plans retain ordinary batch admission.
        constexpr auto f32 = kalandra::NumericType::Float32;
        auto add = runtime.loadNumericGraphKernel(
            std::string(path) + "/kernel0-stage5.metallib", *runtime.device(), f32, {2, 0},
            std::array{f32, f32}, {}, std::array<std::uint32_t, 2>{0, 1},
            std::array<std::uint8_t, 2>{0, 0});
        CHECK(add);
        using K = m::NumericInstructionKind;
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
          const unsigned stages = scenario == 3 ? 66 : 5;
          std::vector<std::array<std::uint32_t, 2>> operands(stages);
          std::vector<m::NumericInstruction> code;
          for (unsigned i = 0; i < stages; ++i) {
            operands[i] = {i, i};
            const auto cost = scenario == 1 || (scenario == 2 && i == 2) ? 0u : 1u << 30;
            code.push_back({K::Map, *add, operands[i], {}, i + 1, 1, cost});
          }
          auto plan = runtime.prepareNumericProgram(1, code, stages, f32, false);
          CHECK(plan);
          const auto before = runtime.stats();
          {
            auto result = runtime.executeNumericProgram(*runner, *plan, std::array{inputs[0]});
            CHECK(result);
            const auto after = runtime.stats();
            const auto commands = scenario == 0 ? (enabled ? 1u : stages) : (stages + 3) / 4;
            CHECK(after.submissions - before.submissions == commands &&
                  after.dispatches - before.dispatches == stages);
            auto expectedCells = data.values[0];
            for (unsigned i = 0; i < stages; ++i)
              for (auto &value : expectedCells)
                value = value + value;
            const auto actual = download(runtime, *result);
            CHECK(actual.size() == expectedCells.size() &&
                  std::memcmp(actual.data(), expectedCells.data(), actual.size() * 4) == 0);
          }
          plan = {};
          empty(*runner);
          clean();
        }
        const std::array<std::uint32_t, 2> original{0, 0};
        const std::array<m::NumericInstruction, 1> scalarCode{
            {{K::Map, *add, original, {}, 1, 1, 32}}};
        auto scalar = runtime.prepareNumericProgram(1, scalarCode, 1, f32, true);
        CHECK(scalar);
        auto result = runtime.executeNumericProgram(*runner, *scalar, std::array{inputs[0]});
        CHECK(!result && result.error().kind == TaskFailure::Fault &&
              result.error().code == std::int64_t(m::Error::Invalid));
      }
      empty(*runner);
      clean();
      for (unsigned scenario = 0; scenario < 2; ++scenario) {
        runtime.holdCompletions(true);
        const auto before = runtime.stats().submissions;
        std::atomic<bool> done{false};
        std::expected<m::Buffer, Failure> result = std::unexpected(Failure{});
        std::jthread worker([&] {
          result = program.launch(runtime, *runner, inputs, transformer::epsilon, data.scale);
          done = true;
        });
        waitFor([&] {
          return runtime.stats().submissions == before + 1 && runner->stats().externalPending == 1;
        });
        CHECK(!done && runtime.stats().inFlight == 1 && runtime.stats().bytes > baseline.bytes);
        CHECK(bool(runtime.progressionCommandReservations()) == !enabled);
        if (scenario)
          runtime.failNextCompletion();
        runtime.holdCompletions(false);
        worker.join();
        if (scenario)
          CHECK(!result && result.error().kind == TaskFailure::Fault &&
                result.error().code == std::int64_t(m::Error::Device));
        else {
          CHECK(result && download(runtime, *result) == expected);
          result = m::Buffer{};
        }
        empty(*runner);
        clean();
      }
      for (unsigned mode = 0; mode < 4; ++mode) {
        Shared shared;
        ParentInput packet{&runtime, &program, &inputs, data.scale, mode, &shared};
        Invocation context;
        alignas(m::Buffer) std::byte slot[sizeof(m::Buffer)];
        Failure failure;
        const auto before = runtime.stats().submissions;
        const bool held = mode != 0;
        if (held)
          runtime.holdCompletions(true);
        std::jthread worker([&] {
          failure = runner->run(parent, {205, &packet, sizeof(packet)}, m::Runtime::bufferLayout(),
                                context, slot);
        });
        if (held) {
          waitFor([&] { return runtime.stats().submissions == before + 1; });
          if (mode == 1)
            waitFor([&] { return shared.cancelled.load(); });
          if (mode == 2)
            waitFor([&] { return shared.externalReady.load(); });
          CHECK(runtime.progressionCommandReservations() >
                0); // Other progress sources force bounded execution.
          if (mode == 2)
            CHECK(shared.external.signal());
          runtime.holdCompletions(false);
        }
        worker.join();
        empty(*runner);
        const auto count = runtime.stats().submissions - before;
        if (mode == 1)
          CHECK(failure.kind == TaskFailure::DeadlineExceeded && count == 1);
        else {
          CHECK(failure.kind == TaskFailure::None && count == (enabled && mode == 0 ? 1u : 5u));
          auto *value = std::launder(reinterpret_cast<m::Buffer *>(slot));
          CHECK(download(runtime, *value) == expected);
          value->~Buffer();
        }
        clean();
      }
    }
    for (unsigned i = 0; i < 7; ++i) {
      auto actual = download(runtime, inputs[i]);
      CHECK(actual.size() == data.values[i].size() &&
            std::memcmp(actual.data(), data.values[i].data(), actual.size() * 4) == 0);
    }
    empty(*runner);
    clean();
  }
  const auto s = runtime.stats();
  CHECK(!s.bytes && !s.metalBuffers && !s.hostBackings && !s.deviceBackings && !s.argumentBytes &&
        !s.argumentPacks && !s.inFlight);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  for (bool enabled : {false, true}) {
    test(argv[1], 1, 64, enabled, false);
    test(argv[1], 3, 7, enabled, false);
    test(argv[1], 16, 512, enabled, false);
    test(argv[1], 64, 512, enabled, true);
  }
  std::puts("Closed GPU exact outputs, captures, escaped ownership, allocation rollback, held "
            "device failures, source deadlines and independent progress passed");
}
