#include "Program.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
using K = NumericProgramKind;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> result, const char *message) {
  if (!result) {
    std::fprintf(stderr, "%s: error %d\n", message, int(result.error()));
    throw std::runtime_error(message);
  }
  return std::move(*result);
}
std::uint64_t bits(NumericDescription type, unsigned value) {
  return type.type == NumericType::Float32   ? std::bit_cast<std::uint32_t>(float(value))
         : type.type == NumericType::Float64 ? std::bit_cast<std::uint64_t>(double(value))
                                             : value;
}
std::vector<std::byte> encoded(NumericDescription type, std::span<const unsigned> values) {
  std::vector<std::byte> result(values.size() * type.bytes());
  for (std::size_t i = 0; i < values.size(); ++i) {
    auto word = bits(type, values[i]);
    std::memcpy(result.data() + i * type.bytes(), &word, type.bytes());
  }
  return result;
}
TensorValue upload(Runtime &runtime, NumericDescription type, std::span<const unsigned> values,
                   std::span<const std::uint64_t> shape) {
  auto raw = encoded(type, values);
  TensorValue value{take(runtime.uploadBytes(raw), "upload program input"), type.type};
  value.rank = shape.size();
  std::copy(shape.begin(), shape.end(), value.dimensions.begin());
  return value;
}
std::expected<TensorValue, Error> execute(Runtime &runtime, ProgramExecution execution) {
  for (;;) {
    auto command = execution.next(runtime);
    if (!command)
      return std::unexpected(command.error());
    if (!*command) {
      if (execution.finished())
        return execution.result();
      continue;
    }
    auto &dispatch = **command;
    auto result = runtime.execute(dispatch.kernel, dispatch.inputs, dispatch.parameters,
                                  dispatch.outputBytes, dispatch.groups);
    if (!result)
      return std::unexpected(result.error());
    auto accepted = execution.accept(runtime, std::move(*result));
    if (!accepted)
      return std::unexpected(accepted.error());
  }
}
ProgramKernel mapKernel(Runtime &runtime, const std::filesystem::path &path, NumericType type) {
  return {take(runtime.loadKernel(path), "load map stage"), type, {type}, {}, {0}, false};
}
void numeric(Runtime &runtime, const std::filesystem::path &maps,
             const std::filesystem::path &primitives, NumericDescription type) {
  auto first = mapKernel(runtime, maps / type.name / "kernel0.spv", type.type);
  auto second = mapKernel(runtime, maps / type.name / "kernel1.spv", type.type);
  ProgramKernel reduce{
      take(runtime.loadKernel(primitives / "reduce" / type.name / "kernel0.spv"), "load reduction"),
      type.type,
      {},
      {},
      {},
      false};
  ProgramKernel matrix{
      take(runtime.loadKernel(primitives / "matmul" / type.name / "kernel0.spv"), "load matrix"),
      type.type,
      {},
      {},
      {},
      false};
  std::array<ProgramInstruction, 2> code{
      {{K::Map, first, {0}, {}, {}, 1}, {K::Map, second, {1}, {}, {}, 2}}};
  auto program = take(Program::prepare(runtime, 1, code, 2, type.type, false), "prepare maps");
  for (unsigned rank : {0u, 1u, 3u, 8u}) {
    std::vector<unsigned> values(rank ? 65 : 1), expected(values.size());
    for (unsigned i = 0; i < values.size(); ++i) {
      values[i] = i % 20;
      expected[i] = values[i] * 2 + 3;
    }
    std::array<std::uint64_t, 8> dimensions{1, 1, 1, 1, 1, 1, 1, 1};
    if (rank)
      dimensions[rank - 1] = values.size();
    auto input = upload(runtime, type, values, std::span(dimensions).first(rank));
    std::array inputs{input};
    auto invocation = take(program.start(runtime, inputs, {}), "start maps");
    input = {};
    inputs = {};
    auto result = take(execute(runtime, std::move(invocation)), "execute owned maps");
    check(!result.scalar && result.rank == rank &&
              std::equal(result.shape().begin(), result.shape().end(), dimensions.begin()),
          "tensor rank/shape preserved");
    check(take(runtime.downloadBytes(result.data), "map result") == encoded(type, expected),
          "independent map reference");
  }
  {
    std::array<std::uint64_t, 3> shape{2, 0, 9};
    std::array inputs{upload(runtime, type, {}, shape)};
    auto result = take(execute(runtime, take(program.start(runtime, inputs, {}), "empty start")),
                       "empty maps");
    check(result.data.bytes() == 0 && result.rank == 3 && result.dimensions[1] == 0,
          "empty tensor geometry preserved");
  }
  auto reductionCode = std::vector<ProgramInstruction>(code.begin(), code.end());
  reductionCode.push_back({K::Reduce, reduce, {2}, {}, {}, 3});
  auto total = take(Program::prepare(runtime, 1, reductionCode, 3, type.type, true), "prepare sum");
  for (unsigned count : {0u, 1u, 3u}) {
    std::vector<unsigned> values(count, 1);
    std::array<std::uint64_t, 1> shape{count};
    std::array inputs{upload(runtime, type, values, shape)};
    auto result = take(execute(runtime, take(total.start(runtime, inputs, {}), "sum start")),
                       "resident map/reduction");
    check(result.scalar && !result.rank, "resident scalar result");
    std::array expected{count * 5};
    check(take(runtime.downloadBytes(result.data), "sum result") == encoded(type, expected),
          "independent map/reduction reference");
  }
  std::array<ProgramInstruction, 4> matrixCode{{{K::MatrixShape, {}, {0, 1}, {}, {}},
                                                {K::Matmul, matrix, {0, 1}, {}, {}, 2},
                                                {K::Map, first, {2}, {}, {}, 3},
                                                {K::Map, second, {3}, {}, {}, 4}}};
  auto product =
      take(Program::prepare(runtime, 2, matrixCode, 4, type.type, false), "prepare matrix program");
  std::array<unsigned, 6> a{1, 2, 1, 2, 1, 2}, b{1, 2, 2, 1, 1, 2};
  std::array<std::uint64_t, 2> as{2, 3}, bs{3, 2};
  std::array inputs{upload(runtime, type, a, as), upload(runtime, type, b, bs)};
  auto result = take(execute(runtime, take(product.start(runtime, inputs, {}), "matrix start")),
                     "resident matrix/map program");
  std::array<unsigned, 4> expected{15, 15, 15, 21};
  check(take(runtime.downloadBytes(result.data), "matrix program result") ==
            encoded(type, expected),
        "independent matrix/map reference");
  auto malformed = code;
  malformed[0].output = UINT32_MAX - 1;
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false), "sparse ID budget");
  check(!Program::prepare(runtime, 1, code, 2, type.type, false, {.maxMetadataBytes = 1}),
        "metadata budget admission");
  malformed = code;
  malformed[0].kernel->reads = std::vector<std::uint32_t>{1};
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "out-of-range physical input projection rejected");
  malformed[0].kernel->reads = std::vector<std::uint32_t>{0, 0};
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "duplicate physical input projection rejected");
  malformed[0].kernel->reads = std::vector<std::uint32_t>{};
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "physical binding count checked against SPIR-V");
  malformed = code;
  malformed[0].scalarSources = {0};
  malformed[0].kernel->scalarTypes = {type.type};
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "parameter word count checked against SPIR-V");
  malformed = code;
  malformed[0].kernel->scalarBuffers = {1};
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "tensor and device-scalar entry ABIs are distinct");
  malformed = code;
  malformed[0].kernel->type =
      type.type == NumericType::Float32 ? NumericType::Float64 : NumericType::Float32;
  check(!Program::prepare(runtime, 1, malformed, 2, type.type, false),
        "result type checked against typed entry ABI");
}
void branches(Runtime &runtime) {
  const auto type = *describeNumeric(NumericType::UInt8);
  std::array<unsigned, 3> a{1, 2, 3};
  std::array<unsigned, 2> b{4, 5};
  std::array<std::uint64_t, 1> as{3}, bs{2};
  auto left = upload(runtime, type, a, as), right = upload(runtime, type, b, bs);
  std::array<ProgramInstruction, 6> code{{{K::Branch, {}, {0}, {}, {}, UINT32_MAX, 4},
                                          {K::EqualShape, {}, {1, 1}, {}, {}},
                                          {K::Alias, {}, {1}, {}, {}, 3},
                                          {K::Jump, {}, {}, {}, {}, UINT32_MAX, 6},
                                          {K::EqualShape, {}, {1, 2}, {}, {}},
                                          {K::Alias, {}, {2}, {}, {}, 3}}};
  auto plan = take(Program::prepare(runtime, 3, code, 3, type.type, false), "prepare branches");
  for (unsigned flag : {0u, 1u}) {
    auto condition = upload(runtime, type, std::span(&flag, 1), {});
    condition.scalar = true;
    std::array inputs{condition, left, right};
    auto result = execute(runtime, take(plan.start(runtime, inputs, {}), "branch start"));
    if (flag)
      check(result && result->data.bytes() == 3, "unselected shape fault is not evaluated");
    else
      check(!result && result.error() == Error::Shape, "selected shape fault retained");
  }
  code[0].target = 0;
  check(!Program::prepare(runtime, 3, code, 3, type.type, false), "cyclic control rejected");
}
void scalars(Runtime &runtime, const std::filesystem::path &root, NumericDescription type) {
  ProgramKernel kernel{take(runtime.loadKernel(root / type.name / "kernel0.spv"), "scalar kernel"),
                       type.type,
                       {},
                       {type.type},
                       {},
                       true};
  std::array<ProgramInstruction, 1> code{{{K::Map, kernel, {}, {0}, {}, 0}}};
  auto plan = take(Program::prepare(runtime, 0, code, 0, type.type, true), "scalar program");
  std::array words{bits(type, 7)};
  auto invocation = take(plan.start(runtime, {}, words), "scalar start");
  words = {0};
  auto result = take(execute(runtime, std::move(invocation)), "scalar execution");
  std::array<unsigned, 1> expected{17};
  check(result.scalar && !result.rank &&
            take(runtime.downloadBytes(result.data), "scalar output") == encoded(type, expected),
        "owned source-width host scalar capture and zero resident inputs");
  if (type.bits < 64) {
    words = {UINT64_C(1) << type.bits};
    auto invalid = execute(runtime, take(plan.start(runtime, {}, words), "bad scalar start"));
    check(!invalid && invalid.error() == Error::Invalid, "invalid high scalar bits rejected");
  }
  if (type.family != NumericFamily::BinaryFloat) {
    words = {integerMaximum(type.type)};
    auto overflow = execute(runtime, take(plan.start(runtime, {}, words), "overflow scalar start"));
    check(!overflow && overflow.error() == Error::Arithmetic, "uniform scalar overflow retained");
  }
}
int run(int argc, char **argv) {
  if (argc != 3 && argc != 4 && argc != 5)
    return 2;
  auto runtime =
      take(Runtime::create(
               {.deviceIndex = argc >= 4 ? unsigned(std::strtoul(argv[3], nullptr, 10)) : 0}),
           "runtime");
  for (auto type : numericTypes) {
    numeric(*runtime, argv[1], argv[2], type);
    if (argc == 5)
      scalars(*runtime, argv[4], type);
    check(!runtime->retainedBytes(), "program data retired");
    std::printf("PASS: %.*s resident map/reduction/matrix programs, ranks 0/1/3/8\n",
                int(type.name.size()), type.name.data());
  }
  branches(*runtime);
  check(!runtime->retainedBytes() && !runtime->validationErrors(), "program cleanup/validation");
  std::printf("PASS: invocation-local GPU programs on %s\n", runtime->device().name.c_str());
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
