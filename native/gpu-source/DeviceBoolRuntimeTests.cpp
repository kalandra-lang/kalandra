// Physical runtime ABI checks against an unmodified compiler-generated UInt8
// identity kernel. Bool mode must validate bytes rather than reinterpret UInt8.
#if defined(KALANDRA_BOOL_METAL)
#include "../metal/Metal.h"
namespace g = kalandra::runtime::metal;
#elif defined(KALANDRA_BOOL_CUDA)
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
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> result, const char *message) {
  if (!result)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(result.error())));
  return std::move(*result);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    g::Options options;
#if defined(KALANDRA_BOOL_METAL)
    options.maxCachedDeviceBytes = options.maxCachedUploadBytes = 0;
    options.cachePreparedOutputs = false;
#endif
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(options), "create runtime"));
#if defined(KALANDRA_BOOL_METAL)
    auto device = take(runtime->device(), "physical Metal device");
    std::cout << "Physical device: " << device.name() << '\n';
    auto kernel = take(runtime->loadNumericGraphKernel(argv[1], device, NumericType::UInt8, {0, 1},
                                                       {}, std::array{NumericType::UInt8}, {}, {}),
                       "load identity kernel");
    const std::array<std::uint64_t, 1> zero{};
    const std::array code{g::NumericInstruction{NumericProgramKind::Map, kernel, {}, zero, 0}};
    auto boolean = take(runtime->prepareNumericProgram(0, code, 0, NumericType::UInt8, true, true),
                        "prepare Bool identity");
    auto numeric = take(runtime->prepareNumericProgram(0, code, 0, NumericType::UInt8, true),
                        "prepare UInt8 identity");
    check(!runtime->prepareNumericProgram(0, code, 0, NumericType::Float32, true, true) &&
              !runtime->prepareNumericProgram(0, code, 0, NumericType::UInt8, false, true),
          "invalid Bool result type/shape admitted");
    const auto execute = [&](auto &core, auto group, bool isBool, auto captures) {
      return runtime->executeNumericProgram(core, group, isBool ? boolean : numeric, {}, captures);
    };
    const auto hold = [&](bool value) { runtime->holdCompletions(value); };
    const auto held = [&] { return runtime->stats().inFlight != 0; };
#else
    const auto device = runtime->device();
    check(device.discrete || device.integrated, "software GPU is not physical qualification");
    std::cout << "Physical device: " << device.name << '\n';
    auto async = g::AsyncRuntime::create(runtime);
    check(bool(async), "create asynchronous runtime");
    g::ProgramKernel schema;
    schema.kernel = take(runtime->loadKernel(argv[1]), "load identity shader");
    schema.type = NumericType::UInt8;
    schema.scalar = true;
    schema.scalarTypes = {NumericType::UInt8};
    schema.reads = std::vector<std::uint32_t>{};
    auto contract = take(g::kernelContract(schema, NumericProgramKind::Map), "identity contract");
    schema.kernel = take(runtime->loadKernel(argv[1], &contract), "typed identity shader");
    const std::array code{g::ProgramInstruction{NumericProgramKind::Map, schema, {}, {0}, {}, 0}};
    auto boolean = take(g::Program::prepare(*runtime, 0, code, 0, NumericType::UInt8, true,
                                            {.booleanResult = true}),
                        "prepare Bool identity");
    auto numeric = take(g::Program::prepare(*runtime, 0, code, 0, NumericType::UInt8, true),
                        "prepare UInt8 identity");
    check(boolean.booleanResult() && !numeric.booleanResult() &&
              !g::Program::prepare(*runtime, 0, code, 0, NumericType::Float32, true,
                                   {.booleanResult = true}) &&
              !g::Program::prepare(*runtime, 0, code, 0, NumericType::UInt8, false,
                                   {.booleanResult = true}),
          "Bool result identity or shape validation lost");
    const auto execute = [&](auto &core, auto group, bool isBool, auto captures) {
      return async->executeProgram(core, group, isBool ? boolean : numeric, {}, captures);
    };
    const auto hold = [&](bool value) { async->holdCompletions(value); };
    const auto held = [&] { return async->stats().held != 0; };
#endif
    for (unsigned workers : {1u, 4u}) {
      for (auto byte : {0u, 1u, 2u, 255u, 256u}) {
        for (bool isBool : {true, false}) {
          auto core = TaskCore::create({.workers = workers});
          check(bool(core), "create task core");
          auto group = core->openRoot();
          const bool invalid = byte > 255 || (isBool && byte > 1);
          std::vector<TaskHandle> tasks(invalid ? 1 : 4);
          const std::array<std::uint64_t, 1> captures{byte};
          for (auto &task : tasks) {
            task = execute(*core, group, isBool, std::span(captures));
            check(bool(task), "submit typed scalar program");
          }
          check(core->close(group) && core->drain(), "drain scalar programs");
          for (auto task : tasks) {
            auto failure = core->failure(task);
            check(invalid ? failure.kind == TaskFailure::Fault && failure.code == 3
                          : failure.kind == TaskFailure::None,
                  "scalar byte validation changed: byte=" + std::to_string(byte) + " Bool=" +
                      std::to_string(isBool) + " kind=" + std::to_string(int(failure.kind)) +
                      " code=" + std::to_string(failure.code));
            if (invalid)
              continue;
            Invocation wrong;
            std::uint8_t untouched = 77;
            check(!core->observe(
                      task, isBool ? *numericScalarLayout(NumericType::UInt8) : boolScalarLayout(),
                      wrong, &untouched) &&
                      untouched == 77,
                  "forged Bool/UInt8 result layout accepted");
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
              Invocation context;
              if (isBool) {
                bool output = false;
                check(core->observe(task, boolScalarLayout(), context, &output) &&
                          output == (byte != 0),
                      "Bool result/repeated observation lost");
              } else {
                std::uint8_t output = 0;
                check(core->observe(task, *numericScalarLayout(NumericType::UInt8), context,
                                    &output) &&
                          output == byte,
                      "numeric UInt8 compatibility lost");
              }
            }
          }
          if (invalid) {
            core->cancel(group);
            check(core->failure(group).kind == TaskFailure::Fault && core->failure(group).code == 3,
                  "late cancellation replaced recorded Bool/capture fault");
          }
          check(core->release(group), "release scalar results");
          auto stats = core->stats();
          check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes,
                "Bool scalar execution retained task resources");
#if defined(KALANDRA_BOOL_METAL)
          auto gpu = runtime->stats();
          check(!gpu.bytes && !gpu.inFlight && !gpu.metalBuffers && !gpu.stagingBuffers,
                "Bool execution retained GPU resources");
#else
          check(!runtime->retainedBytes() && !async->stats().pending &&
                    !async->stats().captureBytes,
                "Bool execution retained GPU resources");
#endif
        }
      }
      // GPU work may have finished, but a fault is not recorded before its real
      // external acknowledgement. Cancellation must retain the existing precedence.
      for (bool deadline : {false, true}) {
        auto core = TaskCore::create({.workers = workers});
        auto group = core->openRoot();
        hold(true);
        const std::array<std::uint64_t, 1> captures{2};
        auto task = execute(*core, group, true, std::span(captures));
        check(task && core->close(group), "submit held Bool outcome");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!held() && std::chrono::steady_clock::now() < until) {
          core->step();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool reached = held();
        if (reached)
          core->cancel(group, deadline);
        hold(false);
        check(core->drain() && reached && core->failure(task).kind == TaskFailure::Cancelled &&
                  core->failure(group).kind ==
                      (deadline ? TaskFailure::DeadlineExceeded : TaskFailure::Cancelled) &&
                  core->release(group),
              "Bool cancellation/deadline precedence lost");
      }
    }
#if !defined(KALANDRA_BOOL_METAL) && !defined(KALANDRA_BOOL_CUDA)
    check(!runtime->validationErrors(), "Bool execution generated Vulkan validation errors");
#endif
    std::cout << "PASS Bool GPU result identity, canonical/invalid bytes, UInt8 compatibility, "
                 "concurrency, cached reads, cancellation/deadlines and fault ordering\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
