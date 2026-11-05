// Run compiler-generated sorting with raw scalar inputs. No floating arithmetic
// or source-level conversion may canonicalize NaNs before/after the tested sort.
#if defined(KALANDRA_SORT_METAL)
#include "../metal/Metal.h"
namespace g = kalandra::runtime::metal;
#elif defined(KALANDRA_SORT_CUDA)
#include "../cuda/Async.h"
namespace g = kalandra::runtime::cuda;
#else
#include "../vulkan/Async.h"
namespace g = kalandra::runtime::vulkan;
#endif
#include <algorithm>
#include <array>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
namespace {
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, g::Error> result, const char *message) {
  if (!result)
    throw std::runtime_error(std::string(message) + ": " + std::to_string(int(result.error())));
  return std::move(*result);
}
#if defined(KALANDRA_SORT_METAL)
using Program = g::NumericExecutable;
Program prepare(g::Runtime &runtime, NumericType type, unsigned frame, char **files) {
  auto device = take(runtime.device(), "Metal device");
  const auto constant = [&](const char *file, NumericType t) {
    return take(runtime.loadNumericGraphKernel(file, device, t, {0, 1}, {}, std::array{t}, {}, {}),
                "load scalar capture map");
  };
  auto values = constant(files[0], type), index = constant(files[1], NumericType::Int64);
  const std::array types{type, type, type, type, NumericType::Int64, NumericType::UInt64};
  const std::array<std::uint32_t, 6> reads{0, 1, 2, 3, 4, 5};
  const std::array<std::uint8_t, 6> roles{1, 1, 1, 1, 1, 0};
  const std::vector<NumericType> states(frame, NumericType::UInt64);
  auto kernel = take(runtime.loadNumericGraphKernel(files[2], device, type, {6, frame}, types,
                                                    states, reads, roles),
                     "load device sort");
  const std::array<std::uint32_t, 5> inputs{0, 1, 2, 3, 4};
  const std::array<std::uint64_t, 1> zero{};
  const std::vector<std::uint64_t> initial(frame);
  std::array<g::NumericInstruction, 6> code;
  for (unsigned i = 0; i < 5; ++i)
    code[i] = {NumericProgramKind::Map, i == 4 ? index : values, {}, zero, i};
  code[5] = {NumericProgramKind::DeviceControl, kernel, inputs, initial, 5};
  return take(runtime.prepareNumericProgram(0, code, 5, type, true), "prepare device sort");
}
#else
using Program = g::Program;
Program prepare(g::Runtime &runtime, NumericType type, unsigned frame, char **files) {
  const auto load = [&](const char *file, NumericType t, bool control) {
    g::ProgramKernel schema;
    schema.kernel = take(runtime.loadKernel(file), "load scalar/sort shader");
    schema.type = t;
    schema.scalar = true;
    schema.scalarTypes = control ? std::vector<NumericType>(frame, NumericType::UInt64)
                                 : std::vector<NumericType>{t};
    if (control) {
      schema.tensorTypes = {type, type, type, type, NumericType::Int64, NumericType::UInt64};
      schema.scalarBuffers = {1, 1, 1, 1, 1, 0};
      schema.reads = std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5};
    } else
      schema.reads = std::vector<std::uint32_t>{};
    auto contract = take(g::kernelContract(schema, control ? NumericProgramKind::DeviceControl
                                                           : NumericProgramKind::Map),
                         "typed scalar/sort contract");
    schema.kernel = take(runtime.loadKernel(file, &contract), "load typed scalar/sort shader");
    return schema;
  };
  auto values = load(files[0], type, false), index = load(files[1], NumericType::Int64, false);
  auto kernel = load(files[2], type, true);
  std::array<g::ProgramInstruction, 6> code;
  for (unsigned i = 0; i < 5; ++i)
    code[i] = {NumericProgramKind::Map, i == 4 ? index : values, {}, {i}, {}, i};
  code[5] = {NumericProgramKind::DeviceControl, kernel, {0, 1, 2, 3, 4}, {},
             std::vector<std::uint64_t>(frame), 5};
  return take(Program::prepare(runtime, 0, code, 5, type, true), "prepare device sort");
}
#endif
} // namespace
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  try {
    const auto type = resolveNumeric(argv[1]);
    check(bool(type), "unknown element type");
    const auto info = *describeNumeric(*type);
    const auto frame = unsigned(std::stoul(argv[2]));
    check(frame && frame < 300000, "invalid frame count");
    g::Options options;
#if defined(KALANDRA_SORT_METAL)
    options.maxCachedDeviceBytes = options.maxCachedUploadBytes = 0;
    options.cachePreparedOutputs = false;
#endif
    auto runtime = std::shared_ptr<g::Runtime>(take(g::Runtime::create(options), "create runtime"));
#if defined(KALANDRA_SORT_METAL)
    auto device = take(runtime->device(), "physical Metal device");
    std::cout << "Physical device: " << device.name() << '\n';
#else
    const auto device = runtime->device();
    check(device.discrete || device.integrated, "software GPU is not qualification");
    std::cout << "Physical device: " << device.name << '\n';
    auto async = g::AsyncRuntime::create(runtime);
    check(bool(async), "create asynchronous runtime");
#endif
    auto program = prepare(*runtime, *type, frame, argv + 3);
    const auto mask = info.bits == 64 ? UINT64_MAX : (std::uint64_t(1) << info.bits) - 1;
    const auto sign = std::uint64_t(1) << (info.bits - 1);
    const auto key = [&](std::uint64_t raw) {
      if (info.family == NumericFamily::BinaryFloat)
        return raw & sign ? ~raw & mask : raw ^ sign;
      return info.family == NumericFamily::SignedInteger ? raw ^ sign : raw;
    };
    const std::array<std::uint64_t, 24> edges{0,
                                              1,
                                              UINT64_MAX,
                                              0x8000000000000000ULL,
                                              0x7ff0000000000000ULL,
                                              0xfff0000000000000ULL,
                                              0x7ff0000000000001ULL,
                                              0xfff0000000000001ULL,
                                              0x7ff8000000000042ULL,
                                              0xfff8000000000042ULL,
                                              0x80000000,
                                              0x7f800000,
                                              0xff800000,
                                              0x7f800001,
                                              0xff800001,
                                              0x7fc00042,
                                              0xffc00042,
                                              0x00800000,
                                              0x0010000000000000ULL,
                                              0x8000000000000001ULL,
                                              0x7f,
                                              0x80,
                                              0x7fff,
                                              0x8000};
    std::uint64_t seed = 0x27953a71;
    unsigned executions = 0;
    for (unsigned workers : {1u, 4u}) {
      auto core = TaskCore::create({.workers = workers});
      check(bool(core), "create task core");
      for (unsigned trial = 0; trial < 40; ++trial) {
        std::array<std::uint64_t, 4> values;
        for (unsigned i = 0; i < 4; ++i) {
          seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
          values[i] = (trial < edges.size() ? edges[(trial + i * 5) % edges.size()] : seed) & mask;
        }
        auto expected = values;
        std::sort(expected.begin(), expected.end(),
                  [&](auto a, auto d) { return key(a) < key(d); });
        auto group = core->openRoot();
        std::array<TaskHandle, 4> tasks;
        for (unsigned i = 0; i < 4; ++i) {
          std::vector<std::uint64_t> captures(values.begin(), values.end());
          captures.push_back(i);
#if defined(KALANDRA_SORT_METAL)
          captures.resize(5 + frame);
          tasks[i] = runtime->executeNumericProgram(*core, group, program, {}, captures);
#else
          tasks[i] = async->executeProgram(*core, group, program, {}, captures);
#endif
          check(bool(tasks[i]), "submit raw-bit sort");
        }
        check(core->close(group) && core->drain(), "drain raw-bit sort");
        for (unsigned i = 0; i < 4; ++i) {
          std::uint64_t raw = 0;
          Invocation context;
          check(core->failure(tasks[i]).kind == TaskFailure::None &&
                    core->observe(tasks[i], *numericScalarLayout(*type), context, &raw) &&
                    raw == expected[i],
                "sort changed raw bits, lost elements or differs from independent ordering");
          ++executions;
        }
        check(core->release(group), "release raw-bit sort group");
        auto task = core->stats();
        check(!task.records && !task.groups && !task.externalPending && !task.retainedBytes,
              "raw-bit sort retained task resources");
#if defined(KALANDRA_SORT_METAL)
        auto stats = runtime->stats();
        check(!stats.bytes && !stats.inFlight && !stats.metalBuffers && !stats.stagingBuffers,
              "raw-bit sort retained device resources");
#else
        check(!runtime->retainedBytes() && !async->stats().pending && !async->stats().captureBytes,
              "raw-bit sort retained device resources");
#endif
      }
    }
#if !defined(KALANDRA_SORT_METAL) && !defined(KALANDRA_SORT_CUDA)
    check(!runtime->validationErrors(), "raw-bit sort generated Vulkan validation errors");
#endif
    std::cout << "PASS: " << executions
              << " raw-bit source sorts, independent ordering, "
                 "edge/random payload preservation, workers 1/4, concurrent indices and cleanup\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
