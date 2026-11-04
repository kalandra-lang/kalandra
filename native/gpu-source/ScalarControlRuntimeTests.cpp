// Physical runtime regression driver. Arguments are the generated int64
// finite-sum, infinite-loop and checked-fault control-stage shaders, each with
// one int64 parameter. Their private frame size is read from the artifact ABI.
// Build kalandra-{vulkan,cuda}-scalar-control-tests; hardware execution is explicit.
#if defined(KALANDRA_CONTROL_CUDA)
#include "../cuda/Async.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Async.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
g::Program prepare(g::Runtime &runtime, const char *file) {
  auto k = take(runtime.loadKernel(file), "load control shader");
  const auto words = k.parameterWords();
  check(words >= 4 && words % 2 == 0 && (k.inputCount() == 1 || k.inputCount() == 2),
        "private control layout");
  const bool arena = k.inputCount() == 2;
  g::ProgramKernel schema;
  schema.kernel = k;
  schema.type = NumericType::Int64;
  schema.tensorTypes = {NumericType::Int64};
  schema.scalarBuffers = {1};
  if (arena) {
    schema.tensorTypes.push_back(NumericType::UInt64);
    schema.scalarBuffers.push_back(0);
  }
  schema.scalar = true;
  schema.scalarTypes.assign((words - 2) / 2, NumericType::UInt64);
  schema.reads = arena ? std::vector<std::uint32_t>{0, 1} : std::vector<std::uint32_t>{0};
  auto contract = take(g::kernelContract(schema, arena ? NumericProgramKind::DeviceControl
                                                       : NumericProgramKind::ScalarControl),
                       "control contract");
  schema.kernel = take(runtime.loadKernel(file, &contract), "load typed control contract");
  g::ProgramInstruction instruction{arena ? NumericProgramKind::DeviceControl
                                          : NumericProgramKind::ScalarControl,
                                    schema,
                                    {0},
                                    {},
                                    std::vector<std::uint64_t>((words - 2) / 2),
                                    1};
  return take(g::Program::prepare(runtime, 1, std::array{instruction}, 1, NumericType::Int64, true),
              "prepare control program");
}
g::TensorValue input(g::Runtime &runtime, std::int64_t n) {
  g::TensorValue v;
  v.type = NumericType::Int64;
  v.scalar = true;
  v.data = take(runtime.uploadBytes(std::as_bytes(std::span(&n, 1))), "upload scalar argument");
  return v;
}
void release(TaskCore &core, GroupHandle group, g::AsyncRuntime &async) {
  check(core.release(group), "release root");
  auto s = core.stats();
  auto a = async.stats();
  check(!s.records && !s.groups && !s.externalPending && !s.retainedBytes && !a.pending &&
            !a.captureBytes,
        "retained checkpoint owners");
}
void success(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program,
             unsigned workers, bool concurrent) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  check(bool(core), "create core");
  auto group = core->openRoot();
  std::array<TaskHandle, 4> tasks;
  const unsigned count = concurrent ? 4 : 1;
  for (unsigned i = 0; i < count; ++i) {
    auto value = input(runtime, 100 * (i + 1));
    tasks[i] = async.executeProgram(*core, group, program, std::array{value}, {});
    check(bool(tasks[i]), "submit control invocation");
  }
  check(core->close(group) && core->drain(), "drain scalar program");
  for (unsigned i = 0; i < count; ++i) {
    Invocation context;
    std::int64_t result = 0, n = 100 * (i + 1);
    check(core->failure(tasks[i]).kind == TaskFailure::None &&
              core->observe(tasks[i], *numericScalarLayout(NumericType::Int64), context, &result) &&
              result == n * (n + 1) / 2,
          "concurrent/reused control result mismatch");
  }
  release(*core, group, async);
  check(runtime.retainedBytes() == 0, "device state retained after result retirement");
}
void cancel(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program,
            unsigned workers, bool deadline) {
  TaskLimits limits;
  limits.workers = workers;
  auto core = TaskCore::create(limits);
  auto group = core->openRoot();
  auto value = input(runtime, 1000);
  async.holdCompletions(true);
  auto task = async.executeProgram(*core, group, program, std::array{value}, {});
  check(bool(task) && core->close(group), "submit cancel program");
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!async.stats().held && std::chrono::steady_clock::now() < end) {
    core->step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool held = async.stats().held == 1;
  if (held)
    check(core->cancel(group, deadline), "cancel held chunk");
  async.holdCompletions(false);
  check(held && core->drain(), "bounded cancellation settlement");
  check(core->failure(task).kind == TaskFailure::Cancelled &&
            core->failure(group).kind ==
                (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
        "cancel/deadline ordering changed");
  value = {};
  release(*core, group, async);
  check(runtime.retainedBytes() == 0, "cancelled control retained device storage");
}
void fault(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program,
           std::int64_t code) {
  auto core = TaskCore::create();
  auto group = core->openRoot();
  auto value = input(runtime, 1000);
  auto task = async.executeProgram(*core, group, program, std::array{value}, {});
  check(bool(task) && core->close(group) && core->drain(), "drain faulty control");
  check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == code,
        "checked fault not reported");
  core->cancel(group);
  check(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == code,
        "recorded fault replaced");
  value = {};
  release(*core, group, async);
  check(runtime.retainedBytes() == 0, "fault retained device state");
}
// Interrupt after the real fence at each collector phase, not an elapsed-time
// guess. This exercises release of a live private heap during clear/mark/sweep.
void interruptCollection(g::Runtime &runtime, const g::Program &program) {
  std::array<bool, 6> seen{};
  for (auto phase : {2u, 3u, 5u}) {
    auto value = input(runtime, 1000);
    const auto baseline = runtime.retainedBytes();
    bool reached = false;
    {
      auto execution =
          take(program.start(runtime, std::array{value}, {}), "start collection probe");
      for (unsigned step = 0; step < 10000 && !execution.finished(); ++step) {
        auto command = take(execution.next(runtime), "advance collection probe");
        if (!command)
          continue;
        auto &c = *command;
        auto result =
            take(runtime.execute(c.kernel, c.inputs, c.parameters, c.outputBytes, c.groups),
                 "execute collector probe");
        check(result.parameters.size() > 10, "missing device allocator frame");
        const auto capacity = std::uint64_t(result.parameters[3]) |
                              (std::uint64_t(result.parameters[4]) << 32);
        check(capacity == detail::ControlArenaABI::InitialWords,
              "steady temporary arrays grew backing instead of reclaiming dead storage");
        const auto state =
            std::uint64_t(result.parameters[9]) | (std::uint64_t(result.parameters[10]) << 32);
        check(bool(execution.accept(runtime, std::move(result))), "accept collection probe");
        if (state == phase) {
          reached = true;
          seen[phase] = true;
          break;
        }
      }
    }
    check(reached && runtime.retainedBytes() == baseline,
          "collector phase not observed or interrupted heap retained storage");
    value = {};
    check(!runtime.retainedBytes(), "collection probe input retained storage");
  }
  check(seen[2] && seen[3] && seen[5], "missing physical collector interruption coverage");
  std::cout << "PASS: physical clear/mark/sweep interruption and heap retirement\n";
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 4 && argc != 5)
    return 2;
  try {
    const bool arrays = argc == 5 && std::string_view(argv[4]) == "arrays";
    g::Options options;
    if (arrays) {
#if defined(KALANDRA_CONTROL_CUDA)
      // CUDA conservatively charges each allocation at driver granularity.
      // The direct phase probe additionally verifies the logical arena never
      // grows beyond its initial 32 words during 1,000 fresh allocations.
      options.maxBytes = 64 * 1024 * 1024;
#else
      options.maxBytes = 16 * 1024;
#endif
    }
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(options), "create driver"));
    auto device = runtime->device();
    check(device.discrete || device.integrated, "source lifetime probe selected software device");
    std::cout << "Physical device: " << device.name << '\n';
    auto async = g::AsyncRuntime::create(runtime, {.maxCaptureBytes = 64 * 1024});
    check(bool(async), "create async driver");
    auto finite = prepare(*runtime, argv[1]), infinite = prepare(*runtime, argv[2]),
         bad = prepare(*runtime, argv[3]);
    for (auto w : {1u, 4u}) {
      for (unsigned i = 0; i < 10; ++i)
        success(*runtime, *async, finite, w, false);
      success(*runtime, *async, finite, w, true);
      for (auto d : {false, true}) {
        cancel(*runtime, *async, infinite, w, d);
        cancel(*runtime, *async, bad, w, d);
      }
    }
    fault(*runtime, *async, bad, arrays ? 5 : 4);
    if (arrays)
      interruptCollection(*runtime, finite);
#if !defined(KALANDRA_CONTROL_CUDA)
    check(!runtime->validationErrors(), "source control emitted Vulkan validation errors");
#endif
    std::cout << "PASS: prepared reuse, distinct concurrent captures, infinite-loop "
                 "cancellation/deadlines, fault ordering and zero retained resources\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
