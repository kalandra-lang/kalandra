#include "Async.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
using namespace kalandra;
using namespace kalandra::runtime;
namespace v = kalandra::runtime::vulkan;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, v::Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %lld\n", message, (long long)value.error());
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
struct Hold {
  v::AsyncRuntime &async;
  explicit Hold(v::AsyncRuntime &a) : async(a) { async.holdCompletions(true); }
  ~Hold() { async.holdCompletions(false); }
};
void held(TaskCore &core, v::AsyncRuntime &async) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!async.stats().held && std::chrono::steady_clock::now() < end) {
    core.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(async.stats().held == 1, "held transform completion");
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 4)
    return 2;
  const bool validation = argc < 4 || std::string_view(argv[3]) != "--no-validation";
  constexpr std::uint64_t maximum = 128 * 1024 * 1024;
  std::shared_ptr<v::Runtime> runtime =
      take(v::Runtime::create(
               {.validation = validation,
                .deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0,
                .maxBytes = maximum}),
           "runtime");
  const auto root = std::filesystem::path(argv[1]);
  for (const auto kind : {TensorTransformKind::Transpose, TensorTransformKind::SoftmaxRows,
                          TensorTransformKind::RmsNormRows}) {
    v::ProgramKernel shader;
    shader.kernel =
        take(runtime->loadKernel(root / (std::string(tensorTransformName(kind)) + "-float32.spv")),
             "kernel");
    shader.type = NumericType::Float32;
    shader.tensorTypes = {NumericType::Float32};
    shader.scalarBuffers = {0};
    shader.transform = kind;
    if (kind == TensorTransformKind::RmsNormRows)
      shader.scalarTypes = {NumericType::Float32};
    v::ProgramInstruction op{NumericProgramKind::Map, shader, {0}, {}, {}, 1};
    if (kind == TensorTransformKind::RmsNormRows)
      op.constants = {std::bit_cast<std::uint32_t>(0.00001f)};
    auto plan =
        take(v::Program::prepare(*runtime, 1, std::span(&op, 1), 1, NumericType::Float32, false),
             "plan");
    const std::array<float, 6> numbers{1, 2, 3, 4, 5, 6};
    v::TensorValue value{take(runtime->upload(numbers), "input"), NumericType::Float32};
    value.rank = 2;
    value.dimensions = {2, 3};
    const std::array inputs{value};
    const auto baseline = runtime->retainedBytes();
    if (kind != TensorTransformKind::Transpose) {
      auto occupied = take(runtime->reserveHostBytes(maximum - baseline), "fill byte quota");
      auto invocation = take(plan.start(*runtime, inputs, {}), "budget invocation");
      const auto dispatch = invocation.next(*runtime);
      check(!dispatch && dispatch.error() == v::Error::Budget, "scratch uses shared quota");
    }
    check(runtime->retainedBytes() == baseline, "scratch quota rollback");
    for (const unsigned workers : {1u, 2u, 4u}) {
      auto async = v::AsyncRuntime::create(runtime);
      auto core = TaskCore::create({.workers = workers});
      check(bool(async) && bool(core), "async setup");
      for (const bool deadline : {false, true}) {
        auto group = core->openRoot();
        Hold hold(*async);
        async->failNextCompletion();
        const auto task = async->executeProgram(*core, group, plan, inputs, {});
        check(bool(task) && core->close(group), "transform submitted");
        held(*core, *async);
        const auto retained = runtime->retainedBytes();
        check(retained > baseline, "output and scratch retained until acknowledgement");
        check(core->cancel(group, deadline) && !core->settled(group) &&
                  core->stats().externalPending == 1,
              "cancellation retains foreign access");
        check(runtime->retainedBytes() == retained,
              "cancellation cannot release in-flight backing");
        async->holdCompletions(false);
        check(core->drain(), "cancelled drain");
        check(core->failure(group).kind ==
                  (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled),
              "group precedence");
        check(core->failure(task).kind == TaskFailure::Cancelled,
              "cancellation wins injected completion fault");
        check(core->release(group), "group release");
        check(!async->stats().pending && !async->stats().captureBytes && !core->stats().records &&
                  !core->stats().externalPending && runtime->retainedBytes() == baseline,
              "all private backing retired");
      }
    }
  }
  check(!runtime->retainedBytes() && !runtime->validationErrors(), "final cleanup and validation");
  std::printf("PASS: managed transforms, scratch quota rollback, held cancellation/deadlines and "
              "fault precedence on %s\n",
              runtime->device().name.c_str());
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
