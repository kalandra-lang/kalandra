#include "Program.h"
#include "kalandra/NumericKernelName.h"
#include <algorithm>
#include <cstring>
#include <new>
namespace kalandra::runtime::vulkan {
struct ProgramDefinition {
  std::vector<ProgramInstruction> code;
  std::vector<std::uint32_t> uses;
  std::uint32_t inputs = 0, result = 0;
  NumericType resultType;
  bool scalar;
  ProgramLimits limits;
  std::uint64_t invocationBaseBytes = 0;
};
namespace {
using K = NumericProgramKind;
std::expected<std::uint32_t, Error> cells(const TensorValue &value) {
  if (value.rank > value.dimensions.size() || (value.scalar && value.rank))
    return std::unexpected(Error::Invalid);
  for (auto extent : value.shape())
    if (extent > INT64_MAX)
      return std::unexpected(Error::Invalid);
  if (std::find(value.shape().begin(), value.shape().end(), 0) != value.shape().end())
    return 0;
  std::uint64_t count = 1;
  for (auto extent : value.shape()) {
    if (extent > UINT32_MAX / count)
      return std::unexpected(Error::Unsupported);
    count *= extent;
  }
  return static_cast<std::uint32_t>(count);
}
bool sameShape(const TensorValue &a, const TensorValue &b) {
  return a.scalar == b.scalar && a.rank == b.rank &&
         std::equal(a.shape().begin(), a.shape().end(), b.shape().begin());
}
bool computes(K kind) { return kind == K::Map || kind == K::Reduce || kind == K::Matmul; }
bool produces(K kind) { return computes(kind) || kind == K::Alias; }
std::expected<void, Error> validateKernel(const ProgramInstruction &op) try {
  const auto &kernel = *op.kernel;
  if (kernel.kernel.tiledMatmul()) {
    if (op.kind != K::Matmul || kernel.type != NumericType::Float32 || kernel.transform || kernel.scalar ||
        !kernel.scalarTypes.empty() || !kernel.scalarBuffers.empty() ||
        (!kernel.tensorTypes.empty() && kernel.tensorTypes != std::vector<NumericType>{NumericType::Float32, NumericType::Float32}) ||
        (kernel.reads && *kernel.reads != std::vector<std::uint32_t>{0, 1}) ||
        kernel.kernel.inputCount() != 2 || kernel.kernel.parameterWords() != 4 ||
        !kernel.matmulTranspose || kernel.matmulTranspose.inputCount() != 1 ||
        kernel.matmulTranspose.parameterWords() != 6 ||
        kernel.matmulTranspose.entryName() != vulkanTransformKernelName(TensorTransformKind::Transpose, NumericType::Float32))
      return std::unexpected(Error::Invalid);
    return {};
  }
  if (kernel.matmulTranspose)
    return std::unexpected(Error::Invalid);
  if (kernel.transform) {
    const auto kind = *kernel.transform;
    const bool rms = kind == TensorTransformKind::RmsNormRows;
    const bool transpose = kind == TensorTransformKind::Transpose;
    if ((!rms && !transpose && kind != TensorTransformKind::SoftmaxRows) || op.kind != K::Map ||
        kernel.scalar || kernel.kernel.localSize() != 256 || kernel.kernel.parameterWords() != 6 ||
        kernel.kernel.inputCount() != (transpose ? 1u
                                       : rms     ? 3u
                                                 : 2u) ||
        op.inputs.size() != (rms && kernel.scalarTypes.empty() ? 2u : 1u) ||
        (!transpose && kernel.type != NumericType::Float32) ||
        !std::all_of(kernel.tensorTypes.begin(), kernel.tensorTypes.end(),
                     [&](auto type) { return type == kernel.type; }) ||
        kernel.scalarBuffers[0] != 0 || (op.inputs.size() == 2 && kernel.scalarBuffers[1] != 1) ||
        kernel.scalarTypes.size() != (rms && op.inputs.size() == 1 ? 1u : 0u) ||
        !std::all_of(kernel.scalarTypes.begin(), kernel.scalarTypes.end(),
                     [&](auto type) { return type == kernel.type; }))
      return std::unexpected(Error::Invalid);
    if (kernel.reads)
      for (std::size_t i = 0; i < op.inputs.size(); ++i)
        if (kernel.reads->size() != op.inputs.size() || (*kernel.reads)[i] != i)
          return std::unexpected(Error::Invalid);
    try {
      if (kernel.kernel.entryName() != vulkanTransformKernelName(kind, kernel.type))
        return std::unexpected(Error::Invalid);
      return {};
    } catch (const std::bad_alloc &) {
      return std::unexpected(Error::Allocation);
    }
  }
  const auto physical = op.kind == K::Map && kernel.reads ? kernel.reads->size() : op.inputs.size();
  if (kernel.kernel.inputCount() != physical)
    return std::unexpected(Error::Invalid);
  if (kernel.reads) {
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < kernel.reads->size(); ++i) {
      const auto input = (*kernel.reads)[i];
      if (input >= op.inputs.size() || (i && input <= previous) ||
          (op.kind != K::Map && input != i))
        return std::unexpected(Error::Invalid);
      previous = input;
    }
    if (op.kind != K::Map && kernel.reads->size() != op.inputs.size())
      return std::unexpected(Error::Invalid);
  }
  auto contract = kernelContract(kernel, op.kind);
  if (!contract)
    return std::unexpected(contract.error());
  if (kernel.kernel.parameterWords() != contract->parameterWords ||
      kernel.kernel.entryName() != contract->entry)
    return std::unexpected(Error::Invalid);
  return {};
} catch (const std::bad_alloc &) {
  return std::unexpected(Error::Allocation);
}

bool validInstruction(const ProgramInstruction &op) {
  const auto n = op.inputs.size();
  if (op.kind != K::Map && !op.scalarSources.empty())
    return false;
  switch (op.kind) {
  case K::Map:
    return op.kernel && op.kernel->tensorTypes.size() == n &&
           op.kernel->scalarTypes.size() == op.scalarSources.size() + op.constants.size() &&
           op.kernel->scalarBuffers.size() == n &&
           std::all_of(op.kernel->scalarBuffers.begin(), op.kernel->scalarBuffers.end(),
                       [](auto role) { return role <= 1; });
  case K::Reduce:
  case K::Matmul:
    return op.kernel && n == (op.kind == K::Reduce ? 1u : 2u) && op.constants.empty();
  case K::Branch:
  case K::Alias:
    return !op.kernel && n == 1 && op.constants.empty();
  case K::Jump:
  case K::ShapeFault:
    return !op.kernel && !n && op.constants.empty();
  case K::EqualShape:
  case K::MatrixShape:
    return !op.kernel && n == 2 && op.constants.empty();
  case K::Extent:
    return !op.kernel &&
           ((n == 1 && op.constants.size() % 2 == 0) || (n == 2 && op.constants.size() == 2));
  }
  return false;
}
} // namespace
std::expected<KernelContract, Error> kernelContract(const ProgramKernel &kernel, NumericProgramKind kind) {
  const auto numeric = [](auto type) { return describeNumeric(type).has_value(); };
  if (!numeric(kernel.type) || !std::all_of(kernel.tensorTypes.begin(), kernel.tensorTypes.end(), numeric) ||
      !std::all_of(kernel.scalarTypes.begin(), kernel.scalarTypes.end(), numeric))
    return std::unexpected(Error::Invalid);
  KernelContract result;
  if (kernel.transform) {
    const auto transform = *kernel.transform;
    if (transform != TensorTransformKind::Transpose && transform != TensorTransformKind::SoftmaxRows &&
        transform != TensorTransformKind::RmsNormRows)
      return std::unexpected(Error::Invalid);
    result.entry = vulkanTransformKernelName(transform, kernel.type);
    result.inputs = transform == TensorTransformKind::Transpose ? 1 : transform == TensorTransformKind::SoftmaxRows ? 2 : 3;
    result.parameterWords = 6;
    return result;
  }
  if (!computes(kind))
    return std::unexpected(Error::Invalid);
  result.inputs = kind == K::Matmul ? 2 : kind == K::Reduce ? 1 :
                  kernel.reads ? kernel.reads->size() : kernel.tensorTypes.size();
  result.parameterWords = kind == K::Matmul ? 4 : 2;
  if (kind == K::Map)
    for (auto type : kernel.scalarTypes)
      result.parameterWords += describeNumeric(type)->bits == 64 ? 2 : 1;
  result.entry = kind == K::Map ? numericKernelName(kernel.type, kernel.tensorTypes, kernel.scalarTypes)
      : "kalandra_numeric_v1_" + std::string(kind == K::Reduce ? "reduce_" : "matmul_") +
        std::string(describeNumeric(kernel.type)->name);
  if (kind == K::Map && std::any_of(kernel.scalarBuffers.begin(), kernel.scalarBuffers.end(), [](auto role) { return role != 0; })) {
    result.entry += "_d_";
    for (auto role : kernel.scalarBuffers) result.entry += role ? 's' : 't';
  }
  result.entry += "_vk_flat_v1";
  return result;
}
std::expected<Program, Error> Program::prepare(Runtime &runtime, std::uint32_t inputCount,
                                               std::span<const ProgramInstruction> code,
                                               std::uint32_t result, NumericType resultType,
                                               bool scalar, ProgramLimits limits) {
  if (!describeNumeric(resultType) || !limits.maxMetadataBytes)
    return std::unexpected(Error::Invalid);
  std::uint64_t slots = inputCount, bytes = sizeof(ProgramDefinition), peakDispatch = 0;
  const auto charge = [&](std::uint64_t count, std::uint64_t unit) {
    if (bytes > limits.maxMetadataBytes || count > (limits.maxMetadataBytes - bytes) / unit)
      return false;
    bytes += count * unit;
    return true;
  };
  if (!charge(code.size(), sizeof(ProgramInstruction)))
    return std::unexpected(Error::Budget);
  for (std::size_t pc = 0; pc < code.size(); ++pc) {
    const auto &op = code[pc];
    if (!validInstruction(op) || produces(op.kind) != (op.output != UINT32_MAX) ||
        (produces(op.kind) && op.output < inputCount) ||
        ((op.kind == K::Branch || op.kind == K::Jump) &&
         (op.target <= pc || op.target > code.size())))
      return std::unexpected(Error::Invalid);
    if (produces(op.kind))
      slots = std::max(slots, std::uint64_t(op.output) + 1);
    if (!charge(op.inputs.size(), sizeof(std::uint32_t)) ||
        !charge(op.scalarSources.size(), sizeof(std::uint32_t)) ||
        !charge(op.constants.size(), sizeof(std::uint64_t)))
      return std::unexpected(Error::Budget);
    if (op.kernel) {
      const auto &kernel = *op.kernel;
      if (!runtime.owns(kernel.kernel) || (kernel.matmulTranspose && !runtime.owns(kernel.matmulTranspose)))
        return std::unexpected(Error::Residency);
      if (!describeNumeric(kernel.type) || !kernel.kernel.localSize())
        return std::unexpected(Error::Invalid);
      const auto numeric = [](auto type) { return describeNumeric(type).has_value(); };
      if (!std::all_of(kernel.tensorTypes.begin(), kernel.tensorTypes.end(), numeric) ||
          !std::all_of(kernel.scalarTypes.begin(), kernel.scalarTypes.end(), numeric))
        return std::unexpected(Error::Invalid);
      auto valid = validateKernel(op);
      if (!valid)
        return std::unexpected(valid.error());
      if (!charge(kernel.tensorTypes.size() + kernel.scalarTypes.size(), sizeof(NumericType)) ||
          !charge(kernel.scalarBuffers.size(), sizeof(std::uint8_t)) ||
          (kernel.reads && !charge(kernel.reads->size(), sizeof(std::uint32_t))))
        return std::unexpected(Error::Budget);
      const std::uint64_t words = kernel.kernel.parameterWords();
      peakDispatch = std::max(peakDispatch, kernel.kernel.inputCount() * sizeof(Buffer) +
                                                words * 8 + sizeof(ProgramDispatch));
    }
  }
  // Charge invocation slots too: a small code array must not authorize an
  // enormous sparse value ID that is only discovered when a task starts.
  if (!charge(slots, sizeof(TensorValue) + sizeof(std::uint32_t)) || slots > UINT32_MAX ||
      result >= slots)
    return std::unexpected(Error::Budget);
  try {
    auto definition = std::make_shared<ProgramDefinition>();
    definition->code.assign(code.begin(), code.end());
    definition->uses.resize(slots);
    definition->inputs = inputCount;
    definition->result = result;
    definition->resultType = resultType;
    definition->scalar = scalar;
    definition->limits = limits;
    definition->invocationBaseBytes = sizeof(ProgramExecution) +
                                      slots * (sizeof(TensorValue) + sizeof(std::uint32_t)) +
                                      peakDispatch;
    for (const auto &op : code)
      for (auto input : op.inputs) {
        if (input >= slots || definition->uses[input] == UINT32_MAX)
          return std::unexpected(Error::Invalid);
        ++definition->uses[input];
      }
    if (definition->uses[result] == UINT32_MAX)
      return std::unexpected(Error::Invalid);
    ++definition->uses[result];
    Program program;
    program.definition_ = std::move(definition);
    return program;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::uint64_t, Error> Program::invocationBytes(std::size_t scalarCount) const {
  if (!definition_)
    return std::unexpected(Error::Invalid);
  const auto base = definition_->invocationBaseBytes;
  const auto maximum = definition_->limits.maxMetadataBytes;
  if (base > maximum || scalarCount > (maximum - base) / sizeof(std::uint64_t))
    return std::unexpected(Error::Budget);
  return base + scalarCount * sizeof(std::uint64_t);
}
std::optional<NumericType> Program::scalarType() const noexcept {
  if (!definition_ || !definition_->scalar)
    return {};
  return definition_->resultType;
}
std::expected<ProgramExecution, Error>
Program::start(Runtime &runtime, std::span<const TensorValue> inputs,
               std::span<const std::uint64_t> scalars) const {
  if (!definition_ || inputs.size() != definition_->inputs)
    return std::unexpected(Error::Invalid);
  auto bytes = invocationBytes(scalars.size());
  if (!bytes)
    return std::unexpected(bytes.error());
  for (const auto &op : definition_->code)
    if (op.kernel && (!runtime.owns(op.kernel->kernel) ||
                     (op.kernel->matmulTranspose && !runtime.owns(op.kernel->matmulTranspose))))
      return std::unexpected(Error::Residency);
  for (const auto &input : inputs) {
    if (input.host || !runtime.owns(input.data))
      return std::unexpected(Error::Residency);
    auto count = cells(input);
    auto type = describeNumeric(input.type);
    if (!count)
      return std::unexpected(count.error());
    if (!type || std::uint64_t(*count) * type->bytes() != input.data.bytes())
      return std::unexpected(Error::Invalid);
  }
  try {
    ProgramExecution execution;
    execution.definition_ = definition_;
    execution.values_.resize(definition_->uses.size());
    std::copy(inputs.begin(), inputs.end(), execution.values_.begin());
    execution.uses_ = definition_->uses;
    execution.scalars_.assign(scalars.begin(), scalars.end());
    return execution;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
void ProgramExecution::release(std::uint32_t id) {
  if (!--uses_[id])
    values_[id] = {};
}
void ProgramExecution::skip(std::size_t begin, std::size_t end) {
  for (auto pc = begin; pc < end; ++pc)
    for (auto input : definition_->code[pc].inputs)
      release(input);
}
std::expected<std::optional<ProgramDispatch>, Error> ProgramExecution::next(Runtime &runtime) {
  const auto fail = [&](Error error) -> std::expected<std::optional<ProgramDispatch>, Error> {
    error_ = error;
    return std::unexpected(error);
  };
  if (error_)
    return std::unexpected(*error_);
  if (!definition_ || pending_)
    return fail(Error::Invalid);
  try {
    if (pc_ < definition_->code.size()) {
      const auto &op = definition_->code[pc_];
      for (auto input : op.inputs)
        if (!values_[input].data)
          return fail(Error::Invalid);
      if (op.kind == K::Jump) {
        skip(pc_ + 1, op.target);
        pc_ = op.target;
        return std::optional<ProgramDispatch>{};
      }
      if (op.kind == K::ShapeFault)
        return fail(Error::Shape);
      if (op.kind == K::Branch) {
        const auto &condition = values_[op.inputs[0]];
        if (!condition.scalar || condition.type != NumericType::UInt8 ||
            condition.data.bytes() != 1)
          return fail(Error::Invalid);
        auto bits = runtime.downloadBytes(condition.data);
        if (!bits)
          return fail(bits.error());
        const auto value = std::to_integer<unsigned>(bits->front());
        if (value > 1)
          return fail(Error::Invalid);
        release(op.inputs[0]);
        if (!value) {
          skip(pc_ + 1, op.target);
          pc_ = op.target;
        } else
          ++pc_;
        return std::optional<ProgramDispatch>{};
      }
      if (op.kind == K::Alias) {
        if (values_[op.output].data)
          return fail(Error::Invalid);
        values_[op.output] = values_[op.inputs[0]];
        release(op.inputs[0]);
        ++pc_;
        return std::optional<ProgramDispatch>{};
      }
      // A scalar constant map can have no resident inputs.
      const TensorValue empty;
      const auto &a = op.inputs.empty() ? empty : values_[op.inputs[0]];
      if (op.kind == K::EqualShape || op.kind == K::MatrixShape || op.kind == K::Extent) {
        bool valid = false;
        if (op.kind == K::EqualShape)
          valid = sameShape(a, values_[op.inputs[1]]);
        else if (op.kind == K::MatrixShape) {
          const auto &b = values_[op.inputs[1]];
          valid = !a.scalar && !b.scalar && a.rank == 2 && b.rank == 2 &&
                  a.dimensions[1] == b.dimensions[0];
        } else if (op.inputs.size() == 1) {
          valid = true;
          for (std::size_t i = 0; i < op.constants.size(); i += 2)
            valid &=
                op.constants[i] < a.rank && a.dimensions[op.constants[i]] == op.constants[i + 1];
        } else {
          const auto &b = values_[op.inputs[1]];
          valid = op.constants[0] < a.rank && op.constants[1] < b.rank &&
                  a.dimensions[op.constants[0]] == b.dimensions[op.constants[1]];
        }
        if (!valid)
          return fail(Error::Shape);
        for (auto input : op.inputs)
          release(input);
        ++pc_;
        return std::optional<ProgramDispatch>{};
      }
      if (values_[op.output].data)
        return fail(Error::Invalid);
      const auto &schema = *op.kernel;
      ProgramDispatch dispatch{schema.kernel, {}, {}, 0, 0};
      output_ = {};
      output_.type = schema.type;
      std::uint32_t count = 0;
      if (schema.transform) {
        if (a.type != schema.type)
          return fail(Error::Invalid);
        if (a.scalar || a.rank != 2)
          return fail(Error::Shape);
        const auto kind = *schema.transform;
        const bool transpose = kind == TensorTransformKind::Transpose;
        const bool rms = kind == TensorTransformKind::RmsNormRows;
        auto size = cells(a);
        if (!size)
          return fail(size.error());
        count = *size;
        output_.rank = 2;
        output_.dimensions = a.dimensions;
        if (transpose)
          std::swap(output_.dimensions[0], output_.dimensions[1]);
        // Empty metadata can contain large extents. No coordinates are needed
        // on device, but RMS epsilon must still execute its checked preflight.
        std::uint32_t rows = 0, columns = 0;
        if (count) {
          if (a.dimensions[0] > UINT32_MAX || a.dimensions[1] > UINT32_MAX)
            return fail(Error::Unsupported);
          rows = a.dimensions[0];
          columns = a.dimensions[1];
        }
        std::uint32_t epsilon = 0;
        const bool deviceEpsilon = rms && op.inputs.size() == 2;
        if (rms && !deviceEpsilon) {
          std::uint64_t word;
          if (!op.scalarSources.empty()) {
            if (op.scalarSources[0] >= scalars_.size())
              return fail(Error::Invalid);
            word = scalars_[op.scalarSources[0]];
          } else
            word = op.constants[0];
          if (word > UINT32_MAX)
            return fail(Error::Invalid);
          epsilon = word;
        }
        dispatch.inputs = {a.data};
        if (deviceEpsilon) {
          const auto &value = values_[op.inputs[1]];
          if (!value.scalar || value.rank || value.type != schema.type || value.data.bytes() != 4)
            return fail(Error::Invalid);
          dispatch.inputs.push_back(value.data);
        } else if (rms)
          dispatch.inputs.push_back(a.data); // Unread unless device-epsilon flag is set.
        if (!transpose) {
          auto scratch = runtime.blankBytes(std::uint64_t(count) * 4);
          if (!scratch)
            return fail(scratch.error());
          dispatch.inputs.push_back(std::move(*scratch));
        }
        dispatch.parameters = {count, rows, columns, epsilon, deviceEpsilon ? 1u : 0u, UINT32_MAX};
        const auto groups = transpose ? std::uint64_t(rows / 32 + unsigned(rows % 32 != 0)) *
                                            (columns / 32 + unsigned(columns % 32 != 0))
                                      : rows;
        if (groups > UINT32_MAX)
          return fail(Error::Unsupported);
        dispatch.groups = std::max(1u, std::uint32_t(groups));
        dispatch.outputBytes = std::uint64_t(count) * describeNumeric(schema.type)->bytes();
        parameterWords_ = 6;
        pending_ = true;
        return std::optional<ProgramDispatch>(std::move(dispatch));
      } else if (op.kind == K::Map) {
        std::optional<std::size_t> domain;
        for (std::size_t i = 0; i < op.inputs.size(); ++i) {
          const auto &value = values_[op.inputs[i]];
          if (value.type != schema.tensorTypes[i] || value.scalar != bool(schema.scalarBuffers[i]))
            return fail(Error::Invalid);
          if (!schema.scalarBuffers[i]) {
            if (domain && !sameShape(value, values_[op.inputs[*domain]]))
              return fail(Error::Shape);
            domain = i;
          }
        }
        if (schema.reads)
          for (auto input : *schema.reads)
            dispatch.inputs.push_back(values_[op.inputs[input]].data);
        else
          for (auto input : op.inputs)
            dispatch.inputs.push_back(values_[input].data);
        if (schema.scalar == domain.has_value())
          return fail(Error::Invalid);
        if (domain) {
          output_.rank = values_[op.inputs[*domain]].rank;
          output_.dimensions = values_[op.inputs[*domain]].dimensions;
        }
        output_.scalar = schema.scalar;
        auto size = cells(output_);
        if (!size)
          return fail(size.error());
        count = *size;
        dispatch.parameters.push_back(count);
        for (std::size_t i = 0; i < schema.scalarTypes.size(); ++i) {
          std::uint64_t value;
          if (i < op.scalarSources.size()) {
            if (op.scalarSources[i] >= scalars_.size())
              return fail(Error::Invalid);
            value = scalars_[op.scalarSources[i]];
          } else
            value = op.constants[i - op.scalarSources.size()];
          const auto width = describeNumeric(schema.scalarTypes[i])->bits;
          if (width < 64 && value >> width)
            return fail(Error::Invalid);
          dispatch.parameters.push_back(std::uint32_t(value));
          if (width == 64)
            dispatch.parameters.push_back(std::uint32_t(value >> 32));
        }
      } else if (op.kind == K::Reduce) {
        if (a.scalar || a.type != schema.type)
          return fail(Error::Invalid);
        if (!reducing_)
          reduction_ = a;
        auto size = cells(reduction_);
        if (!size)
          return fail(size.error());
        count = std::max(1u, (*size / 2) + (*size % 2));
        output_.rank = 1;
        output_.dimensions[0] = count;
        dispatch.inputs = {reduction_.data};
        dispatch.parameters = {*size};
        reducing_ = true;
      } else if (op.kind == K::Matmul) {
        const auto &b = values_[op.inputs[1]];
        if (a.type != schema.type || b.type != schema.type)
          return fail(Error::Invalid);
        if (a.scalar || b.scalar || a.rank != 2 || b.rank != 2 ||
            a.dimensions[1] != b.dimensions[0])
          return fail(Error::Shape);
        for (auto extent : {a.dimensions[0], a.dimensions[1], b.dimensions[1]})
          if (extent > UINT32_MAX)
            return fail(Error::Unsupported);
        output_.rank = 2;
        output_.dimensions[0] = a.dimensions[0];
        output_.dimensions[1] = b.dimensions[1];
        auto size = cells(output_);
        if (!size)
          return fail(size.error());
        count = *size;
        if (schema.kernel.tiledMatmul() && count && a.dimensions[1] > 1 && b.dimensions[1] > 1 && !matrixReady_) {
          auto rightCount = cells(b);
          if (!rightCount)
            return fail(rightCount.error());
          output_.dimensions[0] = b.dimensions[1];
          output_.dimensions[1] = b.dimensions[0];
          dispatch.kernel = schema.matmulTranspose;
          dispatch.inputs = {b.data};
          dispatch.parameters = {*rightCount, std::uint32_t(b.dimensions[0]), std::uint32_t(b.dimensions[1]), 0, 0, UINT32_MAX};
          const auto groups = ((b.dimensions[0] + 31) / 32) * ((b.dimensions[1] + 31) / 32);
          if (groups > UINT32_MAX)
            return fail(Error::Unsupported);
          dispatch.groups = std::max(1u, std::uint32_t(groups));
          dispatch.outputBytes = std::uint64_t(*rightCount) * 4;
          parameterWords_ = 6;
          pending_ = matrixTransposing_ = true;
          return std::optional<ProgramDispatch>(std::move(dispatch));
        }
        dispatch.inputs = {a.data, matrixReady_ ? matrixRight_.data : b.data};
        dispatch.parameters = {std::uint32_t(a.dimensions[0]), std::uint32_t(a.dimensions[1]),
                               std::uint32_t(b.dimensions[1])};
      } else
        return fail(Error::Invalid);
      dispatch.parameters.push_back(UINT32_MAX);
      dispatch.outputBytes = std::uint64_t(count) * describeNumeric(schema.type)->bytes();
      const auto width = schema.kernel.localSize();
      dispatch.groups = std::max(1u, count / width + unsigned(count % width != 0));
      parameterWords_ = dispatch.parameters.size();
      pending_ = true;
      return std::optional<ProgramDispatch>(std::move(dispatch));
    }
    finished_ = true;
    auto value = result();
    if (!value)
      return fail(value.error());
    return std::optional<ProgramDispatch>{};
  } catch (const std::bad_alloc &) {
    return fail(Error::Allocation);
  }
}
std::expected<void, Error> ProgramExecution::accept(Runtime &runtime, KernelResult result) {
  const auto fail = [&](Error error) -> std::expected<void, Error> {
    error_ = error;
    return std::unexpected(error);
  };
  if (error_)
    return std::unexpected(*error_);
  if (!pending_ || !definition_ || pc_ >= definition_->code.size() ||
      result.parameters.size() != parameterWords_)
    return fail(Error::Invalid);
  if (!runtime.owns(definition_->code[pc_].kernel->kernel) || !runtime.owns(result.output))
    return fail(Error::Residency);
  if (result.parameters.back() != UINT32_MAX)
    return fail(Error::Arithmetic);
  auto size = cells(output_);
  if (!size || !result.output ||
      result.output.bytes() != std::uint64_t(*size) * describeNumeric(output_.type)->bytes())
    return fail(Error::Invalid);
  output_.data = std::move(result.output);
  pending_ = false;
  if (matrixTransposing_) {
    matrixRight_ = std::move(output_);
    matrixTransposing_ = false;
    matrixReady_ = true;
    return {};
  }
  matrixRight_ = {};
  matrixReady_ = false;
  if (reducing_ && *size > 1) {
    reduction_ = std::move(output_);
    return {};
  }
  if (reducing_) {
    output_.rank = 0;
    output_.dimensions = {};
    output_.scalar = true;
    reduction_ = {};
    reducing_ = false;
  }
  const auto &op = definition_->code[pc_];
  values_[op.output] = std::move(output_);
  for (auto input : op.inputs)
    release(input);
  ++pc_;
  return {};
}
std::expected<TensorValue, Error> ProgramExecution::result() const {
  if (error_)
    return std::unexpected(*error_);
  if (!definition_ || !finished_ || pending_)
    return std::unexpected(Error::Invalid);
  const auto &value = values_[definition_->result];
  if (!value.data || value.type != definition_->resultType || value.scalar != definition_->scalar)
    return std::unexpected(Error::Invalid);
  return value;
}
} // namespace kalandra::runtime::vulkan
