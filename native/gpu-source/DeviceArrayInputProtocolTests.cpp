// Physical malformed-wire tests. The source corpus separately proves the
// language-level entry API and Bool task identity; this tests its private bytes.
#if defined(KALANDRA_CONTROL_CUDA)
#include "../cuda/Async.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Async.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include "../runtime/DeviceArrayABI.h"
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> value, const char *message) {
  if (!value)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(value.error())));
  return std::move(*value);
}
g::Program prepare(g::Runtime &runtime, const char *file, NumericType element) {
  auto kernel = take(runtime.loadKernel(file), "load protocol kernel");
  const auto words = kernel.parameterWords();
  check(kernel.inputCount() == 2 && words >= 4 && words % 2 == 0,
        "unexpected protocol shader schema");
  g::ProgramKernel schema;
  schema.kernel = kernel;
  schema.type = element;
  schema.tensorTypes = {NumericType::UInt64, NumericType::UInt64};
  schema.scalarBuffers = {2, 0};
  schema.scalar = true;
  schema.scalarTypes.assign((words - 2) / 2, NumericType::UInt64);
  schema.reads = std::vector<std::uint32_t>{0, 1};
  auto contract =
      take(g::kernelContract(schema, NumericProgramKind::DeviceControl), "protocol contract");
  schema.kernel = take(runtime.loadKernel(file, &contract), "load checked protocol kernel");
  g::ProgramInstruction op{NumericProgramKind::DeviceControl,           schema, {0}, {},
                           std::vector<std::uint64_t>((words - 2) / 2), 1};
  return take(g::Program::prepare(runtime, 1, std::array{op}, 1, element, true),
              "prepare protocol");
}
void run(g::Runtime &runtime, g::AsyncRuntime &async, const g::Program &program,
         NumericType element, std::vector<std::uint64_t> wire, bool valid,
         std::uint64_t expected = 0) {
  auto core = TaskCore::create();
  check(bool(core), "create protocol core");
  auto group = core->openRoot();
  g::TensorValue input;
  input.type = NumericType::UInt64;
  input.rank = 1;
  input.dimensions[0] = wire.size();
  input.data = take(runtime.uploadBytes(std::as_bytes(std::span(wire))), "upload protocol bytes");
  auto task = async.executeProgram(*core, group, program, std::array{input}, {});
  check(bool(task) && core->close(group) && core->drain(), "settle protocol execution");
  const auto failure = core->failure(task);
  if (valid) {
    check(failure.kind == TaskFailure::None, "valid wire failed");
    Invocation context;
    if (element == NumericType::Int64) {
      std::int64_t result = 0;
      check(core->observe(task, *numericScalarLayout(element), context, &result) &&
                std::uint64_t(result) == expected,
            "int wire result differs from oracle");
    } else {
      std::uint8_t result = 0;
      check(core->observe(task, *numericScalarLayout(element), context, &result) &&
                result == expected,
            "Bool wire result differs from oracle");
    }
  } else {
    check(failure.kind == TaskFailure::Fault && failure.code == std::int64_t(g::Error::Invalid),
          "malformed wire did not produce private metadata fault");
    core->cancel(group);
    check(core->failure(group).kind == TaskFailure::Fault,
          "cancellation replaced recorded wire fault");
  }
  input = {};
  check(core->release(group), "release protocol group");
  auto stats = core->stats();
  auto pending = async.stats();
  check(!stats.records && !stats.groups && !stats.externalPending && !stats.retainedBytes &&
            !pending.pending && !pending.captureBytes && runtime.retainedBytes() == 0,
        "protocol execution retained owners");
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  try {
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(), "create runtime"));
    check(runtime->device().discrete || runtime->device().integrated, "physical GPU required");
    auto async = g::AsyncRuntime::create(runtime);
    check(bool(async), "create async runtime");
    auto integers = prepare(*runtime, argv[1], NumericType::Int64);
    auto booleans = prepare(*runtime, argv[2], NumericType::UInt8);
    std::vector<std::uint64_t> wire{DeviceArrayWireABI::Magic, 4, 1, 1, 17};
    run(*runtime, *async, integers, NumericType::Int64, wire, true, 17);
    for (auto field : {0u, 1u, 2u, 3u}) {
      auto bad = wire;
      bad[field] = field == 0 ? 0 : field == 1 ? 11 : field == 2 ? 2 : 0;
      run(*runtime, *async, integers, NumericType::Int64, bad, false);
    }
    auto bad = wire;
    bad[3] = std::uint64_t(UINT32_MAX) + 1;
    run(*runtime, *async, integers, NumericType::Int64, bad, false);
    for (unsigned n = 0; n < 4; ++n)
      run(*runtime, *async, integers, NumericType::Int64, std::vector<std::uint64_t>(n), false);
    wire = {DeviceArrayWireABI::Magic, 4, 0, 1};
    run(*runtime, *async, integers, NumericType::Int64, wire, true, 0);
    wire = {DeviceArrayWireABI::Magic, 11, 1, 1, 1};
    run(*runtime, *async, booleans, NumericType::UInt8, wire, true, 1);
    for (auto value : {2u, 255u}) {
      wire[4] = value;
      run(*runtime, *async, booleans, NumericType::UInt8, wire, false);
    }
#if !defined(KALANDRA_CONTROL_CUDA)
    check(runtime->validationErrors() == 0, "Vulkan validation error");
#endif
    std::cout << "PASS: typed wire headers, actual lengths, empty input, canonical Bool, fault "
                 "precedence and zero owners\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
