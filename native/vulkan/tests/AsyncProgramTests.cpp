#include "../runtime/NumericStorage.h"
#include "Async.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace {
using K = NumericProgramKind;
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> result, const char *message) {
  if (!result) {
    std::fprintf(stderr, "%s: error %d\n", message, int(result.error()));
    throw std::runtime_error(message);
  }
  return std::move(*result);
}
struct Hold {
  v::AsyncRuntime &async;
  explicit Hold(v::AsyncRuntime &runtime) : async(runtime) { async.holdCompletions(true); }
  ~Hold() { async.holdCompletions(false); }
};
void held(TaskCore &core, v::AsyncRuntime &async) {
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!async.stats().held && std::chrono::steady_clock::now() < end) {
    core.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(async.stats().held == 1, "program reaches held stage acknowledgement");
}
v::TensorValue observe(TaskCore &core, TaskHandle task) {
  Invocation invocation;
  alignas(v::TensorValue) std::byte memory[sizeof(v::TensorValue)];
  check(core.observe(task, v::AsyncRuntime::tensorLayout(), invocation, memory), "observe tensor");
  auto *value = std::launder(reinterpret_cast<v::TensorValue *>(memory));
  auto copy = *value;
  value->~TensorValue();
  return copy;
}
void release(TaskCore &core, GroupHandle group, v::AsyncRuntime &async) {
  check(core.drain() && core.release(group), "release program group");
  check(!core.stats().records && !core.stats().externalPending && !core.stats().retainedBytes,
        "program task records retired");
  check(!async.stats().pending && !async.stats().captureBytes, "program capture ledger retired");
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 5)
    return 2;
  bool validation = true;
  std::filesystem::path scalarRoot;
  for (int i = 3; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--no-validation")
      validation = false;
    else if (scalarRoot.empty())
      scalarRoot = argv[i];
    else
      return 2;
  }
  std::shared_ptr<v::Runtime> runtime =
      take(v::Runtime::create(
               {.validation = validation,
                .deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0}),
           "runtime");
  auto root = std::filesystem::path(argv[1]);
  v::ProgramKernel add{take(runtime->loadKernel(root / "float32/kernel1.spv"), "add kernel"),
                       NumericType::Float32,
                       {NumericType::Float32},
                       {},
                       {0},
                       false};
  std::vector<v::ProgramInstruction> code;
  constexpr unsigned stages = 257;
  for (unsigned i = 0; i < stages; ++i)
    code.push_back({K::Map, add, {i}, {}, {}, i + 1});
  auto program = take(v::Program::prepare(*runtime, 1, code, stages, NumericType::Float32, false),
                      "prepare long program");
  std::array<float, 3> initial{1, 2, 3};
  v::TensorValue input{take(runtime->upload(initial), "input"), NumericType::Float32};
  input.rank = 3;
  input.dimensions = {1, 3, 1};
  std::array inputs{input};
  v::TensorValue escaped;
  for (unsigned workers : {1u, 2u, 4u}) {
    auto async = v::AsyncRuntime::create(runtime);
    auto core = TaskCore::create({.workers = workers, .records = 1});
    check(bool(async) && bool(core), "program service/core");
    auto group = core->openRoot();
    const auto before = async->stats().stages;
    auto task = async->executeProgram(*core, group, program, inputs, {});
    check(bool(task) && core->stats().records == 1 && core->close(group) && core->drain(),
          "257 stages fit one task record");
    check(core->failure(task).kind == TaskFailure::None, "long program succeeds");
    escaped = observe(*core, task);
    auto cached = observe(*core, task);
    check(cached.rank == 3 && cached.dimensions == input.dimensions && !cached.scalar,
          "managed tensor metadata");
    check(take(runtime->download(cached.data), "managed result") ==
              std::vector<float>({1 + stages * 3, 2 + stages * 3, 3 + stages * 3}),
          "independent 257-stage result");
    check(async->stats().stages - before == stages, "every stage executed exactly once");
    release(*core, group, *async);
    for (bool deadline : {false, true}) {
      group = core->openRoot();
      Hold hold(*async);
      const auto start = async->stats().stages;
      task = async->executeProgram(*core, group, program, inputs, {});
      check(bool(task) && core->close(group), "held program submit");
      held(*core, *async);
      auto retained = runtime->retainedBytes();
      check(core->cancel(group, deadline) && !core->settled(group) &&
                core->stats().externalPending == 1 && runtime->retainedBytes() == retained,
            "program cancellation waits for safe acknowledgement");
      async->holdCompletions(false);
      check(core->drain() && core->failure(group).kind == (deadline ? TaskFailure::DeadlineExceeded
                                                                    : TaskFailure::Cancelled),
            "program cancellation/deadline precedence");
      check(async->stats().stages - start == 1, "cancellation skips remaining 256 GPU stages");
      release(*core, group, *async);
    }
    // Plans and all caller tensor aliases may disappear while an invocation is
    // still queued. The result remains usable after service/core teardown.
    group = core->openRoot();
    auto localPlan =
        take(v::Program::prepare(*runtime, 1, code, stages, NumericType::Float32, false),
             "temporary plan");
    auto localValue = input;
    localValue.data = take(runtime->upload(initial), "unique captured input");
    std::array localInputs{localValue};
    task = async->executeProgram(*core, group, localPlan, localInputs, {});
    localPlan = {};
    localValue = {};
    localInputs = {};
    check(bool(task) && core->close(group), "facade teardown submit");
    async.reset();
    check(core->drain() && core->failure(task).kind == TaskFailure::None,
          "program definition survives facade/plan teardown");
    escaped = observe(*core, task);
    check(core->release(group), "orphaned program cleanup");
  }
  {
    // Two invocations share a prepared definition but own independent values,
    // use counts and metadata. Both are admitted before the held stage resumes.
    auto async = v::AsyncRuntime::create(runtime);
    auto core = TaskCore::create({.records = 2});
    check(bool(async) && bool(core), "concurrent setup");
    auto group = core->openRoot();
    Hold hold(*async);
    auto first = async->executeProgram(*core, group, program, inputs, {});
    held(*core, *async);
    auto second = async->executeProgram(*core, group, program, inputs, {});
    check(bool(first) && bool(second) && core->close(group), "two program invocations admitted");
    async->holdCompletions(false);
    check(core->drain() && core->failure(first).kind == TaskFailure::None &&
              core->failure(second).kind == TaskFailure::None,
          "round-robin program completion");
    auto a = observe(*core, first), b = observe(*core, second);
    check(take(runtime->download(a.data), "first output") ==
              take(runtime->download(b.data), "second output"),
          "invocation-local use counts and data");
    release(*core, group, *async);
  }
  {
    auto async = v::AsyncRuntime::create(runtime, {.maxCaptureBytes = 1});
    auto core = TaskCore::create();
    check(bool(async) && bool(core), "capture budget setup");
    auto group = core->openRoot();
    auto task = async->executeProgram(*core, group, program, inputs, {});
    check(bool(task) && core->close(group) && core->drain(), "capture budget drain");
    check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 6 &&
              !async->stats().stages,
          "capture metadata rejected before any GPU work");
    release(*core, group, *async);
  }
  {
    v::ProgramKernel multiply{
        take(runtime->loadKernel(root / "uint32/kernel0.spv"), "checked multiply"),
        NumericType::UInt32,
        {NumericType::UInt32},
        {},
        {0},
        false};
    std::array<v::ProgramInstruction, 2> faultCode{
        {{K::Map, multiply, {0}, {}, {}, 1}, {K::ShapeFault, {}, {}, {}, {}}}};
    auto faulty = take(v::Program::prepare(*runtime, 1, faultCode, 1, NumericType::UInt32, false),
                       "prepare ordered faults");
    const std::uint32_t maximum = UINT32_MAX;
    v::TensorValue value{
        take(runtime->uploadBytes(std::as_bytes(std::span(&maximum, 1))), "fault input"),
        NumericType::UInt32};
    std::array faultInputs{value};
    auto async = v::AsyncRuntime::create(runtime);
    auto core = TaskCore::create();
    check(bool(async) && bool(core), "fault setup");
    auto group = core->openRoot();
    auto task = async->executeProgram(*core, group, faulty, faultInputs, {});
    check(bool(task) && core->close(group) && core->drain(), "fault drain");
    check(core->failure(task).kind == TaskFailure::Fault && core->failure(task).code == 4,
          "earlier arithmetic fault wins later shape fault");
    release(*core, group, *async);
  }
  if (!scalarRoot.empty()) {
    auto async = v::AsyncRuntime::create(runtime);
    auto core = TaskCore::create();
    check(bool(async) && bool(core), "native scalar setup");
    for (auto type : numericTypes) {
      v::ProgramKernel kernel{
          take(runtime->loadKernel(scalarRoot / type.name / "kernel0.spv"), "native scalar kernel"),
          type.type,
          {},
          {type.type},
          {},
          true};
      std::array<v::ProgramInstruction, 1> scalarCode{{{K::Map, kernel, {}, {0}, {}, 0}}};
      auto scalar = take(v::Program::prepare(*runtime, 0, scalarCode, 0, type.type, true),
                         "prepare native scalar");
      const auto encode = [&](unsigned value) -> std::uint64_t {
        if (type.type == NumericType::Float32)
          return std::bit_cast<std::uint32_t>(float(value));
        if (type.type == NumericType::Float64)
          return std::bit_cast<std::uint64_t>(double(value));
        return value;
      };
      auto group = core->openRoot();
      std::array words{encode(7)};
      auto task = async->executeProgram(*core, group, scalar, {}, words);
      words = {0};
      check(bool(task) && core->close(group) && core->drain(), "native scalar drain");
      check(core->failure(task).kind == TaskFailure::None, "native scalar success");
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        Invocation context;
        alignas(std::max_align_t) std::array<std::byte, 8> memory{};
        check(core->observe(task, *numericScalarLayout(type.type), context, memory.data()),
              "source/CPU numeric scalar layout");
        std::uint64_t actual = 0;
        std::memcpy(&actual, memory.data(), type.bytes());
        check(actual == encode(17), "exact cached native scalar bits");
      }
      release(*core, group, *async);
    }
  }
  check(take(runtime->download(escaped.data), "escaped program output") ==
            std::vector<float>({772, 773, 774}),
        "program result outlives core/service");
  escaped = {};
  inputs = {};
  input = {};
  check(!runtime->retainedBytes() && !runtime->validationErrors(), "program resource cleanup");
  std::printf("PASS: managed resident programs, 257 stages/one record, cancellation, ownership, "
              "concurrent invocations and fault precedence on %s (Vulkan validation %s)\n",
              runtime->device().name.c_str(), validation ? "enabled" : "disabled");
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
