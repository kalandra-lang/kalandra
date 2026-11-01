#include "Program.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime::vulkan;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T> T take(std::expected<T, Error> value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s: %d\n", message, int(value.error()));
    throw std::runtime_error(message);
  }
  return std::move(*value);
}
std::expected<TensorValue, Error> execute(Runtime &runtime, ProgramExecution invocation) {
  for (;;) {
    auto next = invocation.next(runtime);
    if (!next)
      return std::unexpected(next.error());
    if (!*next) {
      if (invocation.finished())
        return invocation.result();
      continue;
    }
    auto &command = **next;
    auto result = runtime.execute(command.kernel, command.inputs, command.parameters,
                                  command.outputBytes, command.groups);
    if (!result)
      return std::unexpected(result.error());
    auto accepted = invocation.accept(runtime, std::move(*result));
    if (!accepted)
      return std::unexpected(accepted.error());
  }
}
ProgramKernel kernel(Runtime &runtime, const std::filesystem::path &root, TensorTransformKind kind,
                     NumericType type, bool deviceEpsilon = false) {
  ProgramKernel result;
  result.kernel =
      take(runtime.loadKernel(root / (std::string(tensorTransformName(kind)) + "-" +
                                      std::string(describeNumeric(type)->name) + ".spv")),
           "transform kernel");
  result.type = type;
  result.tensorTypes = {type};
  result.scalarBuffers = {0};
  result.transform = kind;
  if (kind == TensorTransformKind::RmsNormRows) {
    if (deviceEpsilon) {
      result.tensorTypes.push_back(type);
      result.scalarBuffers.push_back(1);
    } else
      result.scalarTypes = {type};
  }
  return result;
}
void transpose(Runtime &runtime, const std::filesystem::path &root, NumericDescription type) {
  auto shader = kernel(runtime, root, TensorTransformKind::Transpose, type.type);
  std::array<ProgramInstruction, 1> code{{{NumericProgramKind::Map, shader, {0}, {}, {}, 1}}};
  auto plan = take(Program::prepare(runtime, 1, code, 1, type.type, false), "transpose plan");
  std::vector<std::array<std::uint64_t, 2>> shapes{
      {0, 17},  {17, 0},  {1, 1},     {1, 33},        {33, 1},
      {31, 35}, {33, 65}, {129, 257}, {INT64_MAX, 0}, {0, INT64_MAX}};
  if (type.type == NumericType::Float32)
    shapes.push_back({1, 2097153}); // 65,537 tile groups, including padded physical groups.
  for (auto shape : shapes) {
    auto [rows, columns] = shape;
    auto count = rows && columns ? rows * columns : 0;
    std::vector<std::byte> data(count * type.bytes()), expected(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
      data[i] = std::byte((i * 73 + i / 7 + 91) % 256);
    for (std::uint64_t row = 0; row < rows && count; ++row)
      for (std::uint64_t col = 0; col < columns; ++col)
        std::memcpy(expected.data() + (col * rows + row) * type.bytes(),
                    data.data() + (row * columns + col) * type.bytes(), type.bytes());
    TensorValue input{take(runtime.uploadBytes(data), "transpose input"), type.type};
    input.rank = 2;
    input.dimensions[0] = rows;
    input.dimensions[1] = columns;
    auto result =
        take(execute(runtime, take(plan.start(runtime, std::array{input}, {}), "transpose start")),
             "transpose execute");
    check(result.rank == 2 && result.dimensions[0] == columns && result.dimensions[1] == rows,
          "transpose metadata");
    check(take(runtime.downloadBytes(result.data), "transpose result") == expected,
          "independent bitwise transpose reference");
    check(take(runtime.downloadBytes(input.data), "transpose input unchanged") == data,
          "transpose source immutable");
  }
}
float adjacent(std::vector<float> values) {
  if (values.empty())
    return 0;
  while (values.size() > 1) {
    std::vector<float> next;
    for (std::size_t i = 0; i < values.size(); i += 2)
      next.push_back(i + 1 < values.size() ? values[i] + values[i + 1] : values[i]);
    values = std::move(next);
  }
  return values[0];
}
std::vector<float> reference(std::span<const float> values, bool rms, float epsilon) {
  if (values.empty())
    return {};
  std::vector<float> temporary(values.begin(), values.end()), result(values.size());
  if (rms) {
    for (auto &value : temporary)
      value = value * value;
    const float denominator = std::sqrt(adjacent(temporary) / float(values.size()) + epsilon);
    for (std::size_t i = 0; i < values.size(); ++i)
      result[i] = values[i] / denominator;
  } else {
    bool nan =
        std::any_of(values.begin(), values.end(), [](float value) { return std::isnan(value); });
    const float high = nan ? std::numeric_limits<float>::quiet_NaN()
                           : *std::max_element(values.begin(), values.end());
    for (std::size_t i = 0; i < values.size(); ++i)
      temporary[i] = high == std::numeric_limits<float>::infinity()
                         ? (values[i] == high ? 1.0f : 0.0f)
                         : float(std::exp(double(values[i]) - double(high)));
    const float sum = adjacent(temporary);
    for (std::size_t i = 0; i < values.size(); ++i)
      result[i] = temporary[i] / sum;
  }
  return result;
}
void rows(Runtime &runtime, const std::filesystem::path &root, bool rms, bool deviceEpsilon) {
  const auto kind = rms ? TensorTransformKind::RmsNormRows : TensorTransformKind::SoftmaxRows;
  auto shader = kernel(runtime, root, kind, NumericType::Float32, deviceEpsilon);
  constexpr float epsilon = 0.00001f;
  ProgramInstruction instruction{NumericProgramKind::Map, shader, {0}, {}, {},
                                 deviceEpsilon ? 2u : 1u};
  if (deviceEpsilon)
    instruction.inputs.push_back(1);
  else if (rms)
    instruction.constants = {std::bit_cast<std::uint32_t>(epsilon)};
  std::array code{instruction};
  auto plan = take(Program::prepare(runtime, deviceEpsilon ? 2 : 1, code, instruction.output,
                                    NumericType::Float32, false),
                   "row plan");
  std::vector<std::vector<float>> cases{
      {},
      {0},
      {-0.0f},
      {1, 2, 3},
      {-1000, 0, 1000},
      {INFINITY, INFINITY, 1},
      {-INFINITY, -INFINITY, -INFINITY},
      {1, NAN, -1},
      {-0.0f, 0.0f, -2},
      {std::numeric_limits<float>::max(), 1, -std::numeric_limits<float>::max()}};
  for (unsigned count : {17u, 255u, 256u, 257u, 513u, 1025u}) {
    std::vector<float> data(count);
    for (unsigned i = 0; i < count; ++i)
      data[i] = float(int(i % 23) - 11) * 0.125f;
    cases.push_back(std::move(data));
  }
  for (const auto &values : cases) {
    constexpr unsigned rowCount = 3;
    std::vector<float> data;
    for (unsigned row = 0; row < rowCount; ++row)
      data.insert(data.end(), values.begin(), values.end());
    TensorValue input{take(runtime.upload(data), "row input"), NumericType::Float32};
    input.rank = 2;
    input.dimensions[0] = rowCount;
    input.dimensions[1] = values.size();
    std::vector<TensorValue> inputs(deviceEpsilon ? 2 : 1);
    inputs[0] = input;
    if (deviceEpsilon) {
      TensorValue eps{take(runtime.upload(std::array{epsilon}), "epsilon"), NumericType::Float32};
      eps.scalar = true;
      inputs[1] = std::move(eps);
    }
    auto result =
        take(execute(runtime, take(plan.start(runtime, inputs, {}), "row start")), "row execute");
    auto actual = take(runtime.download(result.data), "row result");
    auto expected = reference(values, rms, epsilon);
    check(actual.size() == data.size() && result.dimensions == input.dimensions, "row shape/size");
    for (unsigned row = 0; row < rowCount; ++row)
      for (std::size_t i = 0; i < expected.size(); ++i) {
        auto value = actual[row * expected.size() + i], oracle = expected[i];
        if (std::isnan(oracle))
          check(std::isnan(value), "row NaN behavior");
        else if (std::isinf(oracle))
          check(value == oracle, "row infinity behavior");
        else if (oracle == 0)
          check(value == 0 && std::signbit(value) == std::signbit(oracle),
                "row signed zero behavior");
        else
          check(std::isfinite(value) &&
                    std::abs(value - oracle) <= 3e-6f * std::max(std::abs(oracle), 1e-6f),
                "row independent float reference");
      }
  }
  if (rms)
    for (auto bits : {0u, 0x80000000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u})
      for (bool empty : {false, true}) {
        std::vector<float> values(empty ? 0 : 3, 1);
        TensorValue input{take(runtime.upload(values), "invalid epsilon input"),
                          NumericType::Float32};
        input.rank = 2;
        input.dimensions[0] = empty ? 0 : 1;
        input.dimensions[1] = 3;
        std::vector<TensorValue> inputs(deviceEpsilon ? 2 : 1);
        inputs[0] = input;
        auto badCode = code;
        if (deviceEpsilon) {
          TensorValue eps{
              take(runtime.upload(std::array{std::bit_cast<float>(bits)}), "bad epsilon"),
              NumericType::Float32};
          eps.scalar = true;
          inputs[1] = std::move(eps);
        } else
          badCode[0].constants = {bits};
        auto badPlan = take(Program::prepare(runtime, deviceEpsilon ? 2 : 1, badCode,
                                             instruction.output, NumericType::Float32, false),
                            "invalid epsilon plan");
        auto result =
            execute(runtime, take(badPlan.start(runtime, inputs, {}), "invalid epsilon start"));
        check(!result && result.error() == Error::Arithmetic,
              "invalid epsilon fails even for empty tensors");
      }
}
int run(int argc, char **argv) {
  if (argc < 2 || argc > 4)
    return 2;
  bool validation = argc < 4 || std::string_view(argv[3]) != "--no-validation";
  auto runtime = take(
      Runtime::create({.validation = validation,
                       .deviceIndex = argc >= 3 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0,
                       .maxBytes = 128 * 1024 * 1024}),
      "runtime");
  for (auto type : numericTypes) {
    transpose(*runtime, argv[1], type);
    check(!runtime->retainedBytes(), "transpose owner cleanup");
  }
  rows(*runtime, argv[1], false, false);
  rows(*runtime, argv[1], true, false);
  rows(*runtime, argv[1], true, true);
  check(!runtime->retainedBytes() && !runtime->validationErrors(), "transform cleanup/validation");
  std::printf("PASS: ten-type bitwise tiled transpose, stable softmax, adjacent-pair RMS, special "
              "values and empty epsilon faults on %s\n",
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
