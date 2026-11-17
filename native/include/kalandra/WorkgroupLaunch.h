#pragma once
#include "NumericType.h"
#include "WorkgroupGeometry.h"
#include <bit>
#include <string>
#include <vector>

namespace kalandra {
// Versioned compiler/runtime contract, deliberately distinct from a pointwise
// map. Expressions describe host-known launch extents, never device data.
enum class WorkgroupShapeAction : std::uint8_t {
  Constant,
  Scalar,
  Extent,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  Negate
};
struct WorkgroupShapeStep {
  WorkgroupShapeAction action;
  std::uint64_t value = 0;
  std::uint32_t input = 0, axis = 0;
};
using WorkgroupShapeExpression = std::vector<WorkgroupShapeStep>;
struct WorkgroupLaunch {
  std::vector<std::uint32_t> groupShape, inputRanks;
  std::vector<WorkgroupShapeExpression> shape;
  std::uint64_t scratchBytes = 0;
  std::vector<WorkgroupShapeExpression> checked{};
};
inline std::optional<std::int64_t>
evaluateWorkgroupShape(std::span<const WorkgroupShapeStep> expression,
                       std::span<const std::uint64_t> scalars,
                       std::span<const std::span<const std::uint64_t>> inputs) {
  if (expression.empty() || expression.size() > 4096)
    return {};
  std::vector<std::int64_t> stack;
  for (const auto &step : expression) {
    using A = WorkgroupShapeAction;
    if (step.action == A::Constant)
      stack.push_back(std::bit_cast<std::int64_t>(step.value));
    else if (step.action == A::Scalar) {
      if (step.input >= scalars.size())
        return {};
      stack.push_back(std::bit_cast<std::int64_t>(scalars[step.input]));
    } else if (step.action == A::Extent) {
      if (step.input >= inputs.size() || step.axis >= inputs[step.input].size() ||
          inputs[step.input][step.axis] > INT64_MAX)
        return {};
      stack.push_back(static_cast<std::int64_t>(inputs[step.input][step.axis]));
    } else {
      if (stack.empty())
        return {};
      auto right = stack.back();
      stack.pop_back();
      std::int64_t result = 0;
      bool invalid = false;
      if (step.action == A::Negate)
        invalid = __builtin_sub_overflow(INT64_C(0), right, &result);
      else {
        if (stack.empty())
          return {};
        auto left = stack.back();
        stack.pop_back();
        switch (step.action) {
        case A::Add:
          invalid = __builtin_add_overflow(left, right, &result);
          break;
        case A::Subtract:
          invalid = __builtin_sub_overflow(left, right, &result);
          break;
        case A::Multiply:
          invalid = __builtin_mul_overflow(left, right, &result);
          break;
        case A::Divide:
        case A::Remainder:
          invalid = !right || (left == INT64_MIN && right == -1);
          if (!invalid)
            result = step.action == A::Divide ? left / right : left % right;
          break;
        default:
          invalid = true;
        }
      }
      if (invalid)
        return {};
      stack.push_back(result);
    }
  }
  return stack.size() == 1 ? std::optional{stack.front()} : std::nullopt;
}
inline std::optional<std::uint32_t> workgroupParameterWords(const WorkgroupLaunch &launch,
                                                            std::span<const NumericType> scalars) {
  if (launch.groupShape.size() > 8 || launch.shape.size() != launch.groupShape.size() ||
      launch.inputRanks.size() > 62 || scalars.size() > 1024)
    return {};
  std::uint32_t words = 2 + 2 * launch.groupShape.size();
  std::uint64_t local = 1;
  for (auto extent : launch.groupShape) {
    if (!extent || extent > 4096 / local)
      return {};
    local *= extent;
  }
  for (auto rank : launch.inputRanks) {
    if (rank > 8)
      return {};
    words += 2 * rank;
  }
  for (auto type : scalars) {
    const auto desc = describeNumeric(type);
    if (!desc)
      return {};
    words += desc->bits == 64 ? 2 : 1;
  }
  return words;
}
inline std::optional<std::uint64_t> workgroupMetadataBytes(const WorkgroupLaunch &launch) {
  if (launch.groupShape.size() > 8 || launch.shape.size() != launch.groupShape.size() ||
      launch.inputRanks.size() > 62 || launch.checked.size() > 4096)
    return {};
  std::uint64_t bytes =
      sizeof(WorkgroupLaunch) +
      (launch.groupShape.size() + launch.inputRanks.size()) * sizeof(std::uint32_t) +
      (launch.shape.size() + launch.checked.size()) * sizeof(WorkgroupShapeExpression);
  std::size_t steps = 0;
  for (const auto *expressions : {&launch.shape, &launch.checked})
    for (const auto &extent : *expressions) {
      if (extent.empty() || extent.size() > 4096 || extent.size() > 300000 - steps)
        return {};
      steps += extent.size();
      bytes += extent.size() * sizeof(WorkgroupShapeStep);
    }
  return bytes;
}
inline std::string workgroupKernelName(NumericType result, std::span<const NumericType> tensors,
                                       std::span<const NumericType> scalars,
                                       const WorkgroupLaunch &launch) {
  std::string name = "kalandra_workgroup_v1_" + std::string(describeNumeric(result)->name);
  for (std::size_t i = 0; i < tensors.size(); ++i)
    name += "_t" + std::string(describeNumeric(tensors[i])->name) + "r" +
            std::to_string(launch.inputRanks[i]);
  for (auto type : scalars)
    name += "_s" + std::string(describeNumeric(type)->name);
  name += "_g";
  for (auto extent : launch.groupShape)
    name += std::to_string(extent) + "x";
  return name;
}
enum class WorkgroupLaunchError { Invalid, Arithmetic, Shape, Resource };
struct WorkgroupPacket {
  WorkgroupGeometry geometry;
  std::vector<std::uint32_t> parameters;
};
inline std::expected<WorkgroupPacket, WorkgroupLaunchError>
prepareWorkgroupLaunch(const WorkgroupLaunch &launch, std::span<const NumericType> scalarTypes,
                       std::span<const std::uint64_t> scalars,
                       std::span<const std::span<const std::uint64_t>> inputs,
                       std::uint32_t maxThreads, std::uint64_t maxElements, std::uint64_t maxGroups,
                       std::uint64_t maxScratch) {
  using Error = WorkgroupLaunchError;
  auto words = workgroupParameterWords(launch, scalarTypes);
  if (!words || !workgroupMetadataBytes(launch) || scalars.size() != scalarTypes.size() ||
      inputs.size() != launch.inputRanks.size())
    return std::unexpected(Error::Invalid);
  if (launch.scratchBytes > maxScratch)
    return std::unexpected(Error::Resource);
  for (std::size_t i = 0; i < inputs.size(); ++i)
    if (inputs[i].size() != launch.inputRanks[i])
      return std::unexpected(Error::Invalid);
  for (std::size_t i = 0; i < scalars.size(); ++i) {
    const auto bits = describeNumeric(scalarTypes[i])->bits;
    if (bits < 64 && scalars[i] >> bits)
      return std::unexpected(Error::Invalid);
  }
  std::array<std::int64_t, 8> shape{};
  for (const auto *expressions : {&launch.shape, &launch.checked})
    for (const auto &expression : *expressions)
      for (const auto &step : expression)
        if ((step.action == WorkgroupShapeAction::Scalar &&
             (step.input >= scalarTypes.size() || scalarTypes[step.input] != NumericType::Int64)) ||
            (step.action == WorkgroupShapeAction::Extent &&
             (step.input >= inputs.size() || step.axis >= inputs[step.input].size())))
          return std::unexpected(Error::Invalid);
  for (const auto &expression : launch.checked)
    if (!evaluateWorkgroupShape(expression, scalars, inputs))
      return std::unexpected(Error::Arithmetic);
  for (std::size_t axis = 0; axis < launch.shape.size(); ++axis) {
    auto extent = evaluateWorkgroupShape(launch.shape[axis], scalars, inputs);
    if (!extent)
      return std::unexpected(Error::Arithmetic);
    shape[axis] = *extent;
  }
  auto geometry = makeWorkgroupGeometry(std::span(shape).first(launch.shape.size()),
                                        launch.groupShape, maxThreads, maxElements,
                                        std::min<std::uint64_t>(maxGroups, UINT32_MAX));
  if (!geometry)
    return std::unexpected(geometry.error() == WorkgroupGeometryError::Extent ? Error::Shape
                                                                              : Error::Resource);
  WorkgroupPacket result{*geometry, {static_cast<std::uint32_t>(geometry->groups)}};
  auto append = [&](std::uint64_t value, bool wide = true) {
    result.parameters.push_back(static_cast<std::uint32_t>(value));
    if (wide)
      result.parameters.push_back(static_cast<std::uint32_t>(value >> 32));
  };
  for (std::size_t axis = 0; axis < launch.shape.size(); ++axis)
    append(geometry->shape[axis]);
  for (auto input : inputs)
    for (auto extent : input)
      append(extent);
  for (std::size_t i = 0; i < scalars.size(); ++i)
    append(scalars[i], describeNumeric(scalarTypes[i])->bits == 64);
  result.parameters.push_back(UINT32_MAX);
  return result;
}
} // namespace kalandra
