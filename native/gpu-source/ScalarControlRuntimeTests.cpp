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
  check(words >= 4 && words % 2 == 0 && k.inputCount() == 1, "private control layout");
  g::ProgramKernel schema;
  schema.kernel = k;
  schema.type = NumericType::Int64;
  schema.tensorTypes = {NumericType::Int64};
  schema.scalarBuffers = {1};
  schema.scalar = true;
  schema.scalarTypes.assign((words - 2) / 2, NumericType::UInt64);
  schema.reads = std::vector<std::uint32_t>{0};
  g::ProgramInstruction instruction{NumericProgramKind::ScalarControl,           schema, {0}, {},
                                    std::vector<std::uint64_t>((words - 2) / 2), 1};
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
void fault(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program) {
  auto core = TaskCore::create();
  auto group = core->openRoot();
  auto value = input(runtime, 1000);
  auto task = async.executeProgram(*core, group, program, std::array{value}, {});
  check(bool(task) && core->close(group) && core->drain(), "drain faulty control");
  check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 4,
        "checked fault not reported");
  core->cancel(group);
  check(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 4,
        "recorded fault replaced");
  value = {};
  release(*core, group, async);
  check(runtime.retainedBytes() == 0, "fault retained device state");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 4)
    return 2;
  try {
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(), "create driver"));
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
    fault(*runtime, *async, bad);
    std::cout << "PASS: prepared reuse, distinct concurrent captures, infinite-loop "
                 "cancellation/deadlines, fault ordering and zero retained resources\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
