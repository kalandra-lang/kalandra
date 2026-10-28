#include "Metal.h"
#include "../include/kalandra/NumericKernelName.h"
#include "ElementwiseLayout.h"
#include "MatmulLayout.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#endif

namespace kalandra::runtime::metal {
struct State {
  Options options;
  std::atomic<std::uint64_t> gpuNanoseconds{0}, encodingNanoseconds{0}, dispatches{0};
  NSArray<id<MTLDevice>> *devices;
  NSArray<id<MTLCommandQueue>> *queues;
  std::vector<std::string> names;
  std::atomic<std::uint64_t> bytes{0}, peakBytes{0}, hostBackings{0}, deviceBackings{0};
  std::atomic<std::uint64_t> metalBuffers{0}, stagingBuffers{0}, inFlight{0}, submissions{0},
      completions{0};
  std::atomic<std::uint64_t> argumentBytes{0}, peakArgumentBytes{0}, argumentPacks{0};
  std::atomic<std::uint64_t> attempts{0}, failAt{0};
  std::atomic<bool> failCompletion{false};
  std::mutex gateMutex;
  std::condition_variable gate;
  bool hold = false;
  bool permit() noexcept {
    auto n = ++attempts;
    return !failAt || n != failAt;
  }
  bool reserve(std::uint64_t n) noexcept {
    auto used = bytes.load();
    do {
      if (n > options.maxBytes - used)
        return false;
    } while (!bytes.compare_exchange_weak(used, used + n));
    auto peak = peakBytes.load();
    while (peak < used + n && !peakBytes.compare_exchange_weak(peak, used + n)) {
    }
    return true;
  }
};
struct MetalAllocation {
  std::shared_ptr<State> state;
  id<MTLBuffer> buffer;
  std::uint64_t bytes;
  bool staging;
  MetalAllocation(std::shared_ptr<State> s, id<MTLBuffer> b, std::uint64_t n, bool stage)
      : state(std::move(s)), buffer(b), bytes(n), staging(stage) {
    ++state->metalBuffers;
    if (staging)
      ++state->stagingBuffers;
  }
  ~MetalAllocation() {
    buffer = nil;
    state->bytes -= bytes;
    --state->metalBuffers;
    if (staging)
      --state->stagingBuffers;
  }
};
struct Storage {
  std::shared_ptr<State> state;
  std::array<std::int64_t, 8> dimensions{};
  std::uint32_t rank = 0, deviceIndex = 0;
  Format format = Format::Float32;
  bool device = false;
  std::uint64_t size = 0, hostReserved = 0;
  std::vector<std::byte> host;
  AllocationLease cpu;
  std::uint64_t cpuOffset = 0;
  std::span<const std::byte> hostView() const noexcept {
    return cpu ? cpu.bytes().subspan(cpuOffset, size) : std::span<const std::byte>(host);
  }
  std::shared_ptr<MetalAllocation> metal;
  Storage(std::shared_ptr<State> s, bool d) : state(std::move(s)), device(d) {
    if (device)
      ++state->deviceBackings;
    else
      ++state->hostBackings;
  }
  ~Storage() {
    metal.reset();
    host.clear();
    if (hostReserved)
      state->bytes -= hostReserved;
    if (device)
      --state->deviceBackings;
    else
      --state->hostBackings;
  }
};
struct KernelState {
  std::shared_ptr<State> registry;
  std::uint32_t index;
  id<MTLComputePipelineState> pipeline;
  bool elementwise, reduction, matrix;
  std::optional<ElementwiseSignature> signature;
  std::array<id<MTLComputePipelineState>, 2> matrixTiles{};
  std::array<id<MTLComputePipelineState>, 6> fusedMatrices{};
  // Backend resource bound, independent of logical signature arity.
  std::array<std::uint32_t, directBufferEntries - 2> reads{};
  std::uint32_t readCount = 2;
  bool projected = false;
  std::vector<std::uint32_t> indirectReads;
  id<MTLFunction> argumentFunction = nil;
  std::uint64_t encodedBytes = 0;
  std::optional<NumericType> numericType{};
  std::optional<TensorTransformKind> transform{};
  std::vector<NumericType> tensorTypes{}, scalarTypes{};
  std::vector<std::uint8_t> scalarBuffers{};
  id<MTLComputePipelineState> blockReduction = nil, tiledMatrix = nil, block32Matrix = nil,
                              block64Matrix = nil, registerMatrix = nil, groupedMatrix = nil,
                              skinnyMatrix = nil;
  bool scalarBuffer(std::size_t i) const { return !scalarBuffers.empty() && scalarBuffers[i]; }
  NumericType tensorType(std::size_t i) const {
    return tensorTypes.empty() ? *numericType : tensorTypes[i];
  }
  NumericType scalarType(std::size_t i) const {
    return scalarTypes.empty() ? *numericType : scalarTypes[i];
  }
  std::uint64_t scalarWords() const {
    if (!signature)
      return 0;
    if (!numericType)
      return signature->scalars;
    if (scalarTypes.empty())
      return std::uint64_t(signature->scalars) *
             (describeNumeric(*numericType)->bytes() == 8 ? 2 : 1);
    std::uint64_t words = 0;
    for (auto type : scalarTypes)
      words += describeNumeric(type)->bytes() == 8 ? 2 : 1;
    return words;
  }
  std::uint64_t parameterWords() const {
    if (transform)
      return 6;
    return matrix && numericType ? 4
           : signature           ? 1 + scalarWords() + unsigned(numericType.has_value())
                                 : 2;
  }
  bool indirect() const { return argumentFunction != nil; }
  std::uint32_t tensorIndex(std::size_t i) const {
    return indirect() ? indirectReads[i] : reads[i];
  }
};
namespace {
std::expected<std::uint64_t, Error> tensorBytes(std::span<const std::int64_t> shape,
                                                Format format) {
  if (shape.size() > 8)
    return std::unexpected(Error::Invalid);
  const auto type = storageNumericType(format);
  if (!type)
    return std::unexpected(Error::Invalid);
  bool zero = false;
  for (auto dimension : shape) {
    if (dimension < 0)
      return std::unexpected(Error::Invalid);
    zero |= dimension == 0;
  }
  if (zero)
    return 0;
  std::uint64_t n = 1;
  for (auto dimension : shape) {
    if (std::uint64_t(dimension) > UINT64_MAX / n)
      return std::unexpected(Error::Invalid);
    n *= std::uint64_t(dimension);
  }
  auto bytes = checkedNumericBytes(*type, n, std::numeric_limits<std::size_t>::max());
  if (!bytes)
    return std::unexpected(Error::Invalid);
  return *bytes;
}
std::expected<std::shared_ptr<MetalAllocation>, Error>
allocateMetal(const std::shared_ptr<State> &state, std::uint32_t index, std::uint64_t n,
              bool staging) {
  if (!state->permit())
    return std::unexpected(Error::Allocation);
  if (n > state->devices[index].maxBufferLength)
    return std::unexpected(Error::Invalid);
  if (!state->reserve(n))
    return std::unexpected(Error::Budget);
  id<MTLBuffer> buffer = nil;
  if (state->permit())
    buffer = [state->devices[index]
        newBufferWithLength:n
                    options:staging ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate];
  if (!buffer) {
    state->bytes -= n;
    return std::unexpected(Error::Allocation);
  }
  try {
    return std::make_shared<MetalAllocation>(state, buffer, n, staging);
  } catch (...) {
    state->bytes -= n;
    return std::unexpected(Error::Allocation);
  }
}
std::expected<std::shared_ptr<Storage>, Error> allocateStorage(const std::shared_ptr<State> &state,
                                                               std::span<const std::int64_t> shape,
                                                               Format format, bool device,
                                                               std::uint32_t index = 0) {
  auto size = tensorBytes(shape, format);
  if (!size)
    return std::unexpected(size.error());
  if (!state->permit())
    return std::unexpected(Error::Allocation);
  std::shared_ptr<Storage> storage;
  try {
    storage = std::make_shared<Storage>(state, device);
  } catch (...) {
    return std::unexpected(Error::Allocation);
  }
  storage->size = *size;
  storage->format = format;
  storage->rank = shape.size();
  storage->deviceIndex = index;
  std::copy(shape.begin(), shape.end(), storage->dimensions.begin());
  if (*size) {
    if (device) {
      const auto width = describeNumeric(*storageNumericType(format))->bytes();
      if (width < 4 && *size > UINT64_MAX - 3)
        return std::unexpected(Error::Invalid);
      const auto physical = width < 4 ? (*size + 3) & ~UINT64_C(3) : *size;
      auto buffer = allocateMetal(state, index, physical, false);
      if (!buffer)
        return std::unexpected(buffer.error());
      storage->metal = *buffer;
    } else {
      if (!state->reserve(*size))
        return std::unexpected(Error::Budget);
      storage->hostReserved = *size;
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      try {
        storage->host.resize(*size);
      } catch (...) {
        return std::unexpected(Error::Allocation);
      }
    }
  }
  return storage;
}
} // namespace
// Immutable submission-owned captures. Requested array payload is budgeted
// separately from tensor/staging bytes; vector/control allocator overhead is
// not.
struct Arguments {
  std::shared_ptr<State> state;
  std::vector<Buffer> tensors;
  std::vector<std::uint32_t> scalarBits;
  std::uint64_t reserved = 0;
  explicit Arguments(std::shared_ptr<State> s) : state(std::move(s)) { ++state->argumentPacks; }
  ~Arguments() {
    state->argumentBytes -= reserved;
    --state->argumentPacks;
  }
};
// Submission-owned workspace, mutated only by its single scheduled task.
struct MatrixGraphArguments {
  std::shared_ptr<State> state;
  std::uint64_t reserved = 0;
  std::size_t inputs = 0;
  bool tensorResult = false;
  std::vector<Buffer> values;
  std::vector<std::uint32_t> ranks, reductions;
  std::vector<Buffer> reduced;
  Kernel join;
  std::shared_ptr<const Arguments> joinArguments;
  std::vector<MatrixProduct> products;
  std::vector<std::uint32_t> mapInputs;
  std::vector<MatrixScalar> broadcasts;
  std::vector<Kernel> maps;
  std::vector<std::shared_ptr<const Arguments>> mapArguments;
  std::vector<std::uint32_t> uses;
  std::vector<std::array<std::int64_t, 8>> shapes;
  explicit MatrixGraphArguments(std::shared_ptr<State> s) : state(std::move(s)) {
    ++state->argumentPacks;
  }
  // Product spans point into this object's owned mapInputs storage.
  MatrixGraphArguments(const MatrixGraphArguments &) = delete;
  MatrixGraphArguments &operator=(const MatrixGraphArguments &) = delete;
  ~MatrixGraphArguments() {
    state->argumentBytes -= reserved;
    --state->argumentPacks;
  }
};
struct NumericProgramArguments {
  struct Instruction {
    NumericInstructionKind kind;
    Kernel kernel;
    std::vector<std::uint32_t> inputs;
    std::vector<std::uint64_t> scalars;
    std::uint32_t output, target;
    std::uint32_t scalarOffset = 0;
    std::shared_ptr<const Arguments> arguments;
  };
  std::shared_ptr<State> state;
  std::vector<Instruction> ownedCode;
  std::span<const Instruction> code;
  std::shared_ptr<const NumericProgramArguments> definition;
  std::vector<std::uint64_t> captures;
  std::uint32_t captureCount = 0, slots = 0;
  std::vector<Buffer> values;
  std::vector<std::uint32_t> uses, retired, origins;
  bool deferRetirement = false;
  std::uint32_t result = 0, index = 0, inputs = 0;
  NumericType element = NumericType::Float32;
  std::size_t pc = 0;
  bool scalar = false, reading = false, condition = false;
  std::uint64_t reserved = 0;
  explicit NumericProgramArguments(std::shared_ptr<State> s) : state(std::move(s)) {
    ++state->argumentPacks;
  }
  ~NumericProgramArguments() {
    state->argumentBytes -= reserved;
    --state->argumentPacks;
  }
  void release(std::uint32_t value) {
    if (uses[value] && !--uses[value]) {
      if (deferRetirement)
        retired.push_back(value);
      else
        values[value] = {};
    }
  }
  void skip(std::size_t first, std::size_t last) {
    for (auto i = first; i < last; ++i)
      for (auto value : code[i].inputs)
        release(value);
  }
};
struct Bridge {
  enum class Kind { Upload, Download, Compute, Elementwise, Reduce, Matmul };
  struct PacketData {
    std::shared_ptr<State> state;
    Buffer input;
    Device target;
    Kind kind;
    Kernel kernel;
    BinaryOperation operation = BinaryOperation::Add;
    Buffer right;
    std::shared_ptr<const Arguments> arguments{};
    std::optional<Error> preparation{};
    Kernel reduction{}, epilogue{}, matrixPost{};
    Buffer replacement{};
    std::shared_ptr<const Arguments> postArguments{}, scalarArguments{};
    Kernel matrixLeftMap{}, matrixRightMap{};
    std::shared_ptr<const Arguments> leftArguments{}, rightArguments{};
    std::shared_ptr<MatrixGraphArguments> graph{};
    bool numericScalar = false;
    std::shared_ptr<NumericProgramArguments> numericProgram{};
  };
  static std::expected<std::shared_ptr<NumericProgramArguments>, Error>
  captureProgram(std::shared_ptr<State> state, std::span<const Buffer> inputs,
                 std::span<const NumericInstruction> code, std::uint32_t resultId,
                 NumericType element, bool scalar, bool prepareOnly = false) noexcept {
    if (!describeNumeric(element) || code.empty() || code.size() > UINT32_MAX ||
        inputs.size() > UINT32_MAX)
      return std::unexpected(Error::Invalid);
    std::uint64_t values = inputs.size(), edges = 0, scalars = 0;
    std::optional<std::uint32_t> index;
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
      const auto &op = code[pc];
      using K = NumericInstructionKind;
      const auto kernel = op.kernel.state_;
      bool valid = false;
      switch (op.kind) {
      case K::Map:
        valid = kernel && kernel->numericType && kernel->signature && kernel->elementwise &&
                !kernel->matrix && !kernel->reduction &&
                op.inputs.size() == kernel->signature->tensors &&
                op.scalars.size() == kernel->signature->scalars;
        if (valid)
          for (std::size_t i = 0; i < op.scalars.size(); ++i) {
            const auto bits = describeNumeric(kernel->scalarType(i))->bits;
            if (bits < 64 && op.scalars[i] >> bits)
              valid = false;
          }
        break;
      case K::Reduce:
        valid = kernel && kernel->numericType && kernel->reduction && !kernel->matrix &&
                op.inputs.size() == 1 && op.scalars.empty();
        break;
      case K::Matmul:
        valid = kernel && kernel->numericType && kernel->matrix && op.inputs.size() == 2 &&
                op.scalars.empty();
        break;
      case K::Branch:
      case K::Alias:
        valid = !kernel && op.inputs.size() == 1 && op.scalars.empty();
        break;
      case K::Jump:
      case K::ShapeFault:
        valid = !kernel && op.inputs.empty() && op.scalars.empty();
        break;
      case K::EqualShape:
      case K::MatrixShape:
        valid = !kernel && op.inputs.size() == 2 && op.scalars.empty();
        break;
      case K::Extent:
        valid = !kernel && ((op.inputs.size() == 1 && op.scalars.size() % 2 == 0) ||
                            (op.inputs.size() == 2 && op.scalars.size() == 2));
        break;
      }
      if (!valid)
        return std::unexpected(Error::Invalid);
      if (op.output != UINT32_MAX)
        values = std::max(values, std::uint64_t(op.output) + 1);
      if (op.inputs.size() > UINT32_MAX - edges || op.scalars.size() > UINT32_MAX - scalars)
        return std::unexpected(Error::Invalid);
      edges += op.inputs.size();
      scalars += op.scalars.size();
      if (op.kernel) {
        const auto &kernel = op.kernel.state_;
        if (!kernel->numericType || kernel->registry != state)
          return std::unexpected(Error::Residency);
        if (index && *index != kernel->index)
          return std::unexpected(Error::Residency);
        index = kernel->index;
      }
      if ((op.kind == NumericInstructionKind::Jump || op.kind == NumericInstructionKind::Branch) &&
          (op.target <= pc || op.target > code.size()))
        return std::unexpected(Error::Invalid);
    }
    if (!index || values > UINT32_MAX || resultId >= values)
      return std::unexpected(Error::Invalid);
    const auto bytes =
        sizeof(NumericProgramArguments) + values * (sizeof(Buffer) + sizeof(std::uint32_t)) +
        code.size() * sizeof(NumericProgramArguments::Instruction) + edges * 4 + scalars * 8;
    try {
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      auto result = std::make_shared<NumericProgramArguments>(state);
      auto used = state->argumentBytes.load();
      do {
        if (used > state->options.maxArgumentBytes ||
            bytes > state->options.maxArgumentBytes - used)
          return std::unexpected(Error::Budget);
      } while (!state->argumentBytes.compare_exchange_weak(used, used + bytes));
      result->reserved = bytes;
      auto peak = state->peakArgumentBytes.load();
      while (peak < used + bytes &&
             !state->peakArgumentBytes.compare_exchange_weak(peak, used + bytes)) {
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      if (!prepareOnly)
        result->values.resize(values);
      result->slots = values;
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->uses.resize(values);
      result->index = *index;
      result->inputs = inputs.size();
      result->result = resultId;
      result->element = element;
      result->scalar = scalar;
      for (std::size_t i = 0; !prepareOnly && i < inputs.size(); ++i) {
        const auto &input = inputs[i].storage_;
        if (!input || !input->device || input->state != state || input->deviceIndex != *index)
          return std::unexpected(Error::Residency);
        result->values[i] = inputs[i];
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->ownedCode.reserve(code.size());
      for (const auto &op : code) {
        const bool produces =
            op.kind == NumericInstructionKind::Map || op.kind == NumericInstructionKind::Reduce ||
            op.kind == NumericInstructionKind::Matmul || op.kind == NumericInstructionKind::Alias;
        if (produces && (op.output < inputs.size() || op.output >= values))
          return std::unexpected(Error::Invalid);
        if (!produces && op.output != UINT32_MAX)
          return std::unexpected(Error::Invalid);
        for (auto value : op.inputs) {
          if (value >= values || result->uses[value] == UINT32_MAX)
            return std::unexpected(Error::Invalid);
          ++result->uses[value];
        }
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        std::vector<std::uint32_t> arguments(op.inputs.begin(), op.inputs.end());
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        std::vector<std::uint64_t> captures(op.scalars.begin(), op.scalars.end());
        result->ownedCode.push_back({op.kind,
                                     op.kernel,
                                     std::move(arguments),
                                     std::move(captures),
                                     op.output,
                                     op.target,
                                     0,
                                     {}});
        auto &instruction = result->ownedCode.back();
        instruction.scalarOffset = result->captureCount;
        if (op.kind == NumericInstructionKind::Map) {
          result->captureCount += op.scalars.size();
          if (prepareOnly) {
            std::vector<Buffer> placeholders(op.inputs.size());
            auto schema = captureNumeric(state, op.kernel, placeholders, op.scalars);
            if (!schema)
              return std::unexpected(schema.error());
            instruction.arguments = *schema;
          }
        }
      }
      if (result->uses[resultId] == UINT32_MAX)
        return std::unexpected(Error::Invalid);
      ++result->uses[resultId];
      result->code = result->ownedCode;
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
  static std::expected<std::shared_ptr<NumericProgramArguments>, Error> instantiateProgram(
      std::shared_ptr<State> state, const std::shared_ptr<const NumericProgramArguments> &plan,
      std::span<const Buffer> inputs, std::span<const std::uint64_t> captures) noexcept {
    if (!plan || inputs.size() != plan->inputs ||
        (!captures.empty() && captures.size() != plan->captureCount))
      return std::unexpected(Error::Invalid);
    if (plan->state != state)
      return std::unexpected(Error::Residency);
    for (const auto &input : inputs)
      if (!input.storage_ || !input.storage_->device || input.storage_->state != state ||
          input.storage_->deviceIndex != plan->index)
        return std::unexpected(Error::Residency);
    if (!captures.empty())
      for (const auto &op : plan->code)
        if (op.kind == NumericInstructionKind::Map)
          for (std::size_t i = 0; i < op.scalars.size(); ++i) {
            const auto bits = describeNumeric(op.kernel.state_->scalarType(i))->bits;
            if (bits < 64 && captures[op.scalarOffset + i] >> bits)
              return std::unexpected(Error::Invalid);
          }
    const auto bytes = sizeof(NumericProgramArguments) +
                       std::uint64_t(plan->slots) * (sizeof(Buffer) + 3 * sizeof(std::uint32_t)) +
                       captures.size_bytes();
    try {
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      auto result = std::make_shared<NumericProgramArguments>(state);
      auto used = state->argumentBytes.load();
      do {
        if (used > state->options.maxArgumentBytes ||
            bytes > state->options.maxArgumentBytes - used)
          return std::unexpected(Error::Budget);
      } while (!state->argumentBytes.compare_exchange_weak(used, used + bytes));
      result->reserved = bytes;
      auto peak = state->peakArgumentBytes.load();
      while (peak < used + bytes &&
             !state->peakArgumentBytes.compare_exchange_weak(peak, used + bytes)) {
      }
      result->definition = plan;
      result->code = plan->code;
      result->index = plan->index;
      result->inputs = plan->inputs;
      result->slots = plan->slots;
      result->result = plan->result;
      result->element = plan->element;
      result->scalar = plan->scalar;
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->values.resize(plan->slots);
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->uses = plan->uses;
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->retired.reserve(plan->slots);
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->origins.assign(plan->slots, UINT32_MAX);
      if (!captures.empty()) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->captures.assign(captures.begin(), captures.end());
      }
      for (std::size_t i = 0; i < inputs.size(); ++i)
        result->values[i] = inputs[i];
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
  static std::expected<std::shared_ptr<const Arguments>, Error>
  capture(std::shared_ptr<State> state, const Kernel &kernel, std::span<const Buffer> tensors,
          std::span<const float> scalars) noexcept {
    const auto expected = kernel.state_ && kernel.state_->signature ? *kernel.state_->signature
                                                                    : ElementwiseSignature{2, 0};
    if (!kernel.state_ || kernel.state_->numericType || !kernel.state_->elementwise ||
        kernel.state_->reduction || kernel.state_->matrix || tensors.size() != expected.tensors ||
        scalars.size() != expected.scalars ||
        (tensors.empty() || tensors.size() > UINT32_MAX || scalars.size() >= UINT32_MAX))
      return std::unexpected(Error::Invalid);
    if (kernel.state_->registry != state)
      return std::unexpected(Error::Residency);
    const auto bytes = std::uint64_t(tensors.size()) * sizeof(Buffer) +
                       std::uint64_t(scalars.size()) * sizeof(std::uint32_t);
    try {
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      auto result = std::make_shared<Arguments>(state);
      auto used = state->argumentBytes.load();
      do {
        if (bytes > state->options.maxArgumentBytes - used)
          return std::unexpected(Error::Budget);
      } while (!state->argumentBytes.compare_exchange_weak(used, used + bytes));
      result->reserved = bytes;
      auto peak = state->peakArgumentBytes.load();
      while (peak < used + bytes &&
             !state->peakArgumentBytes.compare_exchange_weak(peak, used + bytes)) {
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->tensors.assign(tensors.begin(), tensors.end());
      if (!scalars.empty()) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->scalarBits.resize(scalars.size());
        for (std::size_t i = 0; i < scalars.size(); ++i)
          result->scalarBits[i] = std::bit_cast<std::uint32_t>(scalars[i]);
      }
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
  static std::expected<std::shared_ptr<const Arguments>, Error>
  captureNumeric(std::shared_ptr<State> state, const Kernel &kernel,
                 std::span<const Buffer> tensors, std::span<const std::uint64_t> scalars) noexcept {
    if (!kernel.state_ || !kernel.state_->numericType || !kernel.state_->signature ||
        !kernel.state_->elementwise || kernel.state_->matrix || kernel.state_->reduction ||
        tensors.size() != kernel.state_->signature->tensors ||
        scalars.size() != kernel.state_->signature->scalars)
      return std::unexpected(Error::Invalid);
    if (kernel.state_->registry != state)
      return std::unexpected(Error::Residency);
    for (std::size_t i = 0; i < scalars.size(); ++i) {
      const auto bits = describeNumeric(kernel.state_->scalarType(i))->bits;
      if (bits < 64 && scalars[i] >> bits)
        return std::unexpected(Error::Invalid);
    }
    const auto words = kernel.state_->scalarWords();
    const auto bytes = std::uint64_t(tensors.size()) * sizeof(Buffer) + words * 4;
    try {
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      auto result = std::make_shared<Arguments>(state);
      auto used = state->argumentBytes.load();
      do {
        if (used > state->options.maxArgumentBytes ||
            bytes > state->options.maxArgumentBytes - used)
          return std::unexpected(Error::Budget);
      } while (!state->argumentBytes.compare_exchange_weak(used, used + bytes));
      result->reserved = bytes;
      auto peak = state->peakArgumentBytes.load();
      while (peak < used + bytes &&
             !state->peakArgumentBytes.compare_exchange_weak(peak, used + bytes)) {
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->tensors.assign(tensors.begin(), tensors.end());
      if (!scalars.empty()) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->scalarBits.resize(words);
        std::size_t offset = 0;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
          result->scalarBits[offset++] = std::uint32_t(scalars[i]);
          if (describeNumeric(kernel.state_->scalarType(i))->bytes() == 8)
            result->scalarBits[offset++] = std::uint32_t(scalars[i] >> 32);
        }
      }
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
  static std::expected<std::shared_ptr<MatrixGraphArguments>, Error>
  captureGraph(std::shared_ptr<State> state, const Kernel &kernel, std::span<const Buffer> inputs,
               std::span<const MatrixProduct> products, std::span<const Kernel> maps = {},
               std::span<const float> scalars = {}, std::span<const std::uint32_t> roots = {},
               std::span<const MatrixScalar> broadcasts = {}, bool tensorResult = false) noexcept {
    if (tensorResult && (roots.empty() || products.empty() || broadcasts.empty()))
      return std::unexpected(Error::Invalid);
    if ((!maps.empty() && maps.size() != products.size()) || (maps.empty() && !scalars.empty()))
      return std::unexpected(Error::Invalid);
    if (!kernel.state_ ||
        (!kernel.state_->matrix && (roots.empty() || !kernel.state_->reduction)) ||
        inputs.empty() || (products.empty() && roots.empty()) || inputs.size() > UINT32_MAX ||
        products.size() > UINT32_MAX / 2 || inputs.size() + products.size() > UINT32_MAX)
      return std::unexpected(Error::Invalid);
    if (kernel.state_->registry != state)
      return std::unexpected(Error::Residency);
    for (std::size_t i = 0; i < broadcasts.size(); ++i) {
      const auto item = broadcasts[i];
      if (item.stage >= products.size() || item.reduction >= roots.size() || maps.empty() ||
          !maps[item.stage] ||
          (i && (broadcasts[i - 1].stage > item.stage ||
                 (broadcasts[i - 1].stage == item.stage &&
                  broadcasts[i - 1].reduction >= item.reduction))))
        return std::unexpected(Error::Invalid);
      // Source-ordered reductions must all be available before this map runs.
      for (std::size_t r = 0; r <= item.reduction; ++r)
        if (roots[r] >= inputs.size() + item.stage)
          return std::unexpected(Error::Invalid);
    }
    std::uint64_t operandCount = 0;
    for (std::size_t i = 0; i < products.size(); ++i) {
      const auto edge = products[i];
      if (edge.operands.empty())
        continue;
      if (maps.empty() || !maps[i] || edge.operands.front() != edge.left ||
          edge.operands.back() != edge.right || edge.operands.size() > UINT32_MAX - operandCount)
        return std::unexpected(Error::Invalid);
      operandCount += edge.operands.size();
      for (std::size_t j = 0; j < edge.operands.size(); ++j)
        if (edge.operands[j] >= inputs.size() + i ||
            (j && edge.operands[j - 1] >= edge.operands[j]))
          return std::unexpected(Error::Invalid);
    }
    const auto count = inputs.size() + products.size();
    const auto bytes =
        std::uint64_t(roots.size()) * (sizeof(std::uint32_t) + sizeof(Buffer)) +
        std::uint64_t(maps.size()) * (sizeof(Kernel) + sizeof(std::shared_ptr<const Arguments>)) +
        std::uint64_t(count) *
            (sizeof(Buffer) + sizeof(std::uint32_t) + sizeof(std::array<std::int64_t, 8>) +
             (roots.empty() ? 0 : sizeof(std::uint32_t))) +
        std::uint64_t(products.size()) * sizeof(MatrixProduct) +
        operandCount * sizeof(std::uint32_t) +
        std::uint64_t(broadcasts.size()) * sizeof(MatrixScalar);
    try {
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      auto result = std::make_shared<MatrixGraphArguments>(state);
      auto used = state->argumentBytes.load();
      do {
        if (used > state->options.maxArgumentBytes ||
            bytes > state->options.maxArgumentBytes - used)
          return std::unexpected(Error::Budget);
      } while (!state->argumentBytes.compare_exchange_weak(used, used + bytes));
      result->reserved = bytes;
      auto peak = state->peakArgumentBytes.load();
      while (peak < used + bytes &&
             !state->peakArgumentBytes.compare_exchange_weak(peak, used + bytes)) {
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->values.resize(count);
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->products.assign(products.begin(), products.end());
      if (operandCount) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->mapInputs.reserve(operandCount);
        for (auto &edge : result->products) {
          if (edge.operands.empty())
            continue;
          const auto offset = result->mapInputs.size(), size = edge.operands.size();
          result->mapInputs.insert(result->mapInputs.end(), edge.operands.begin(),
                                   edge.operands.end());
          edge.operands = {result->mapInputs.data() + offset, size};
        }
      }
      if (!broadcasts.empty()) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->broadcasts.assign(broadcasts.begin(), broadcasts.end());
      }
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->uses.resize(count);
      if (!state->permit())
        return std::unexpected(Error::Allocation);
      result->shapes.resize(count);
      if (!maps.empty()) {
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->maps.assign(maps.begin(), maps.end());
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->mapArguments.resize(maps.size());
      }
      if (!roots.empty()) {
        if (roots.size() > UINT32_MAX / 2)
          return std::unexpected(Error::Invalid);
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->ranks.resize(count);
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->reductions.assign(roots.begin(), roots.end());
        if (!state->permit())
          return std::unexpected(Error::Allocation);
        result->reduced.resize(roots.size());
      }
      result->inputs = inputs.size();
      result->tensorResult = tensorResult;
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto &input = inputs[i].storage_;
        if (!input || (roots.empty() ? input->rank != 2 : input->rank < 1 || input->rank > 8) ||
            input->format != Format::Float32 || input->size / 4 > UINT32_MAX)
          return std::unexpected(Error::Invalid);
        if (!input->device || input->state != state || input->deviceIndex != kernel.state_->index)
          return std::unexpected(Error::Residency);
        for (unsigned axis = 0; axis < input->rank; ++axis)
          if (std::uint64_t(input->dimensions[axis]) > UINT32_MAX)
            return std::unexpected(Error::Invalid);
        result->values[i] = inputs[i];
        result->shapes[i] = input->dimensions;
        if (!roots.empty())
          result->ranks[i] = input->rank;
      }
      for (std::size_t i = 0; i < products.size(); ++i) {
        const auto edge = products[i];
        const auto output = inputs.size() + i;
        if (edge.left >= output || edge.right >= output)
          return std::unexpected(Error::Invalid);
        const auto a = result->shapes[edge.left], b = result->shapes[edge.right];
        const bool map = !maps.empty() && bool(maps[i]);
        if (map) {
          const auto &mapping = maps[i].state_;
          const auto tensors =
              edge.tensorCount() + std::count_if(broadcasts.begin(), broadcasts.end(),
                                                 [i](auto item) { return item.stage == i; });
          // Small conditional graphs may deliberately use argument buffers to
          // keep the declared interface stable when optimization removes a
          // read.
          const auto binding = mapping->signature.value_or(ElementwiseSignature{2, 0}).binding;
          if (tensors > UINT32_MAX || edge.left > edge.right || !mapping->elementwise ||
              mapping->matrix || mapping->reduction || mapping->projected ||
              mapping->signature.value_or(ElementwiseSignature{2, 0}) !=
                  ElementwiseSignature{std::uint32_t(tensors), std::uint32_t(scalars.size()),
                                       binding} ||
              (a != b || (!roots.empty() && result->ranks[edge.left] != result->ranks[edge.right])))
            return std::unexpected(Error::Invalid);
          for (auto operand : edge.operands)
            if (result->shapes[operand] != a ||
                (!roots.empty() && result->ranks[operand] != result->ranks[edge.left]))
              return std::unexpected(Error::Invalid);
          if (mapping->registry != state || mapping->index != kernel.state_->index)
            return std::unexpected(Error::Residency);
          if (!state->permit())
            return std::unexpected(Error::Allocation);
          std::vector<Buffer> placeholders(tensors);
          auto args = capture(state, maps[i], placeholders, scalars);
          if (!args)
            return std::unexpected(args.error());
          result->mapArguments[i] = *args;
          result->shapes[output] = a;
        } else {
          if (!kernel.state_->matrix ||
              (!roots.empty() &&
               (result->ranks[edge.left] != 2 || result->ranks[edge.right] != 2)) ||
              a[1] != b[0] || std::uint64_t(a[0]) * std::uint64_t(b[1]) > UINT32_MAX)
            return std::unexpected(Error::Invalid);
          result->shapes[output] = {a[0], b[1]};
        }
        if (!roots.empty())
          result->ranks[output] = map ? result->ranks[edge.left] : 2;
        const auto retain = [&](std::uint32_t operand) {
          if (result->uses[operand] == UINT32_MAX)
            return false;
          ++result->uses[operand];
          return true;
        };
        if (edge.operands.empty()) {
          if (!retain(edge.left) || !retain(edge.right))
            return std::unexpected(Error::Invalid);
        } else
          for (auto operand : edge.operands)
            if (!retain(operand))
              return std::unexpected(Error::Invalid);
      }
      for (std::size_t r = 0; r < roots.size(); ++r) {
        const auto root = roots[r];
        if (tensorResult && !std::any_of(broadcasts.begin(), broadcasts.end(),
                                         [r](auto item) { return item.reduction == r; }))
          return std::unexpected(Error::Invalid);
        if (root >= count - (tensorResult ? 1 : 0) || result->uses[root] == UINT32_MAX)
          return std::unexpected(Error::Invalid);
        ++result->uses[root];
      }
      if (tensorResult)
        ++result->uses.back(); // The returned tensor remains live through task
                               // completion.
      for (std::size_t i = inputs.size(); i < (roots.empty() ? count - 1 : count); ++i)
        if (!result->uses[i])
          return std::unexpected(Error::Invalid);
      for (std::size_t i = 0; i < inputs.size(); ++i)
        if (!result->uses[i])
          result->values[i] = {};
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
  // Each invocation owns two alternating outputs and up to 32 immutable count
  // words. A serial encoder orders dispatches and tracked buffer accesses
  // during reuse.
  struct ReductionScratch {
    std::array<std::shared_ptr<MetalAllocation>, 2> outputs{};
    // Apple-family devices pack all words into counts[0]. Other GPUs retain
    // separate offset-zero buffers rather than assuming Apple offset alignment.
    std::array<std::shared_ptr<MetalAllocation>, 32> counts{};
    std::shared_ptr<MetalAllocation> seed, postOutput, postParameters;
  };
  struct Frame {
    PacketData packet;
    Buffer result;
    Buffer matrixLeft{}, matrixRight{}, combinedSource{};
    Kernel combinedKernel{};
    std::shared_ptr<MetalAllocation> combinedParameters;
    std::shared_ptr<MetalAllocation> transformScratch;
    // A final graph stage may share the scalar reduction's command. Keep the
    // graph's immutable captures and every deferred last-use owner through ACK.
    std::shared_ptr<MatrixGraphArguments> combinedGraph;
    // A matrix and its pure post-map may share one uncommitted command.
    // Keep the first stage's owners and budget reservations until acknowledgement.
    Buffer batchedLeft{}, batchedRight{};
    Kernel batchedKernel{};
    std::shared_ptr<MetalAllocation> batchedParameters;
    Kernel fusedPostKernel{};
    std::shared_ptr<MetalAllocation> fusedParameters;
    id<MTLCommandBuffer> pendingCommand = nil;
    // Up to four short, pure graph stages share a command. Graph values keep
    // all tensor owners; these slots keep immutable encoder parameters and
    // indirect resources charged until the whole command acknowledges.
    std::array<std::shared_ptr<MetalAllocation>, 3> graphParameters{}, graphResources{};
    std::array<std::shared_ptr<MetalAllocation>, 3> graphPostParameters{};
    std::array<std::shared_ptr<MetalAllocation>, 4> numericParameters{}, numericResources{},
        numericScratch{};
    std::array<std::uint64_t, 4> numericFlagWords{};
    unsigned numericBatched = 0;
    std::uint64_t numericWork = 0;
    bool numericEligible = false, numericAcknowledging = false;
    bool numericPostWanted = false, numericFused = false;
    std::uint32_t numericResultOrigin = UINT32_MAX;
    std::optional<std::int64_t> numericDeferredError;
    unsigned graphBatched = 0;
    std::uint64_t graphWork = 0;
    Kernel matrixKernel{}, graphKernel{};
    std::size_t graphStage = 0, reductionStage = 0;
    unsigned graphPhase = 0;
    std::shared_ptr<MetalAllocation> joinReadback;
    unsigned matrixStage = 0; // 1: left map, 2: right map, 3: multiply.
    float scalar = 0;
    std::unique_ptr<ReductionScratch> scratch;
    std::shared_ptr<MetalAllocation> staging, resources;
    Error error = Error::Device;
    bool succeeded = false, started = false, active = false;
    explicit Frame(PacketData p) : packet(std::move(p)) {}
    ~Frame() {
      if (active)
        --packet.state->inFlight;
    }
  };
  static bool copy(Invocation &, void *out, const Invocation &, const void *in) noexcept {
    new (out) Buffer(*static_cast<const Buffer *>(in));
    return true;
  }
  static void destroyValue(void *value) noexcept { static_cast<Buffer *>(value)->~Buffer(); }
  static bool construct(Invocation &, void *out, const Packet &packet) {
    new (out) Frame(*static_cast<const PacketData *>(packet.data));
    return true;
  }
  static void destroy(void *frame) noexcept { static_cast<Frame *>(frame)->~Frame(); }
  static const Buffer &tensor(const Frame &frame, std::size_t index) {
    if (frame.packet.numericProgram) {
      const auto &program = *frame.packet.numericProgram;
      return program.values[program.code[program.pc].inputs[index]];
    }
    if (frame.packet.graph && frame.packet.kind == Kind::Elementwise) {
      if (frame.graphPhase == 2)
        return frame.packet.graph->reduced[index];
      const auto edge = frame.packet.graph->products[frame.graphStage];
      const auto tensors = edge.tensorCount();
      if (index < tensors)
        return frame.packet.graph
            ->values[edge.operands.empty() ? (index == 0 ? edge.left : edge.right)
                                           : edge.operands[index]];
      index -= tensors;
      for (auto item : frame.packet.graph->broadcasts)
        if (item.stage == frame.graphStage && index-- == 0)
          return frame.packet.graph->reduced[item.reduction];
      std::terminate(); // Preflight validated the compact binding table.
    }
    return index == 0 && frame.packet.replacement ? frame.packet.replacement
                                                  : frame.packet.arguments->tensors[index];
  }
  static void matrixMaps(PacketData &packet, Kernel left, Kernel right,
                         std::span<const Buffer> inputs, std::span<const float> scalars) noexcept {
    if (!left && !right) {
      if (!inputs.empty() || !scalars.empty())
        packet.preparation = Error::Invalid;
      return;
    }
    const auto &base = packet.kernel.state_;
    for (auto *map : {&left, &right}) {
      if (!*map)
        continue;
      if (!base || !base->matrix || !map->state_ || map->state_->index != base->index) {
        packet.preparation = Error::Invalid;
        return;
      }
      auto args = capture(packet.state, *map, inputs, scalars);
      if (!args) {
        packet.preparation = args.error();
        return;
      }
      (map == &left ? packet.leftArguments : packet.rightArguments) = *args;
    }
    packet.matrixLeftMap = std::move(left);
    packet.matrixRightMap = std::move(right);
  }
  static const ValueLayout layout;
  static const TaskEntry entry, reductionEntry;
  static const TaskEntry &numericEntry(NumericType type) {
    static const auto entries = [] {
      std::array<TaskEntry, 10> result{};
      for (std::size_t i = 0; i < numericTypes.size(); ++i) {
        result[i] = reductionEntry;
        result[i].result = numericScalarLayout(numericTypes[i].type);
      }
      return result;
    }();
    for (std::size_t i = 0; i < numericTypes.size(); ++i)
      if (numericTypes[i].type == type)
        return entries[i];
    return reductionEntry;
  }
  template <NumericType Type> static void completeNumeric(Task &task, const void *bits) {
    NumericValue<Type> value;
    std::memcpy(&value, bits, sizeof(value));
    task.complete(&value);
  }
  static void completeNumeric(Task &task, NumericType type, const void *bits) {
    switch (type) {
#define NUMERIC_COMPLETE(T)                                                                        \
  case NumericType::T:                                                                             \
    completeNumeric<NumericType::T>(task, bits);                                                   \
    return;
      NUMERIC_COMPLETE(Int8)
      NUMERIC_COMPLETE(UInt8)
      NUMERIC_COMPLETE(Int16)
      NUMERIC_COMPLETE(UInt16)
      NUMERIC_COMPLETE(Int32)
      NUMERIC_COMPLETE(UInt32)
      NUMERIC_COMPLETE(Int64)
      NUMERIC_COMPLETE(UInt64) NUMERIC_COMPLETE(Float32) NUMERIC_COMPLETE(Float64)
#undef NUMERIC_COMPLETE
    }
  }
  static id<MTLCommandBuffer> numericReductionCommand(Frame &f) {
    const auto state = f.packet.state;
    const auto index = f.packet.kernel.state_->index;
    const auto type = *f.packet.kernel.state_->numericType;
    const auto width = describeNumeric(type)->bytes();
    if (!state->permit())
      return nil;
    f.scratch.reset(new (std::nothrow) ReductionScratch{});
    if (!f.scratch)
      return nil;
    auto readback = allocateMetal(state, index, std::max(width, 4u), true);
    if (!readback) {
      f.error = readback.error();
      return nil;
    }
    f.staging = *readback;
    std::memset(f.staging->buffer.contents, 0, std::max(width, 4u));
    if (f.packet.numericProgram) {
      auto output =
          allocateStorage(state, std::array<std::int64_t, 1>{1}, *storageFormat(type), true, index);
      if (!output) {
        f.error = output.error();
        return nil;
      }
      f.result = Buffer(*output);
    }
    auto length = f.packet.input.bytes() / width;
    id<MTLBuffer> input = length ? f.packet.input.storage_->metal->buffer : nil;
    unsigned levels = 0;
    for (auto remaining = length; remaining > 1; remaining = (remaining + 1) / 2)
      ++levels;
    for (unsigned level = 0; level < levels; ++level) {
      const auto next = (length + 1) / 2;
      auto &output = f.scratch->outputs[level % 2];
      if (!output) {
        auto allocated = allocateMetal(
            state, index, std::max((next * width + 3) & ~UINT64_C(3), UINT64_C(4)), false);
        if (!allocated) {
          f.error = allocated.error();
          return nil;
        }
        output = *allocated;
      }
      auto parameters = allocateMetal(state, index, 8, true);
      if (!parameters) {
        f.error = parameters.error();
        return nil;
      }
      f.scratch->counts[level] = *parameters;
      auto words = static_cast<std::uint32_t *>((*parameters)->buffer.contents);
      words[0] = std::uint32_t(length);
      words[1] = UINT32_MAX;
      length = next;
    }
    id<MTLCommandBuffer> command = state->permit() ? [state->queues[index] commandBuffer] : nil;
    if (!command)
      return nil;
    length = f.packet.input.bytes() / width;
    for (unsigned level = 0; level < levels; ++level) {
      const auto next = (length + 1) / 2;
      auto output = f.scratch->outputs[level % 2]->buffer;
      if (width < 4) {
        id<MTLBlitCommandEncoder> clear = state->permit() ? [command blitCommandEncoder] : nil;
        if (!clear)
          return nil;
        [clear fillBuffer:output range:NSMakeRange(0, output.length) value:0];
        [clear endEncoding];
      }
      id<MTLComputeCommandEncoder> encoder =
          state->permit() ? [command computeCommandEncoder] : nil;
      if (!encoder)
        return nil;
      [encoder setComputePipelineState:f.packet.kernel.state_->pipeline];
      [encoder setBuffer:input offset:0 atIndex:0];
      [encoder setBuffer:output offset:0 atIndex:1];
      [encoder setBuffer:f.scratch->counts[level]->buffer offset:0 atIndex:2];
      if (state->options.profile)
        ++state->dispatches;
      [encoder dispatchThreadgroups:MTLSizeMake((next + 63) / 64, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
      input = output;
      length = next;
    }
    if (!input && f.packet.numericProgram)
      input = f.staging->buffer;
    if (input) {
      id<MTLBlitCommandEncoder> blit = state->permit() ? [command blitCommandEncoder] : nil;
      if (!blit)
        return nil;
      if (f.packet.numericProgram && width < 4)
        [blit fillBuffer:f.result.storage_->metal->buffer
                   range:NSMakeRange(0, f.result.storage_->metal->bytes)
                   value:0];
      [blit copyFromBuffer:input
               sourceOffset:0
                   toBuffer:f.packet.numericProgram ? f.result.storage_->metal->buffer
                                                    : f.staging->buffer
          destinationOffset:0
                       size:width];
      [blit endEncoding];
    }
    if (!state->permit())
      return nil;
    return command;
  }
  static id<MTLCommandBuffer> reductionCommand(Frame &f, id<MTLCommandBuffer> incoming = nil) {
    auto state = f.packet.state;
    auto index = f.packet.kernel.state_->index;
    if (!state->permit())
      return nil;
    f.scratch.reset(new (std::nothrow) ReductionScratch{});
    if (!f.scratch)
      return nil;
    const bool resident = f.packet.graph && f.graphPhase == 1;
    if (resident) {
      auto output =
          allocateStorage(state, std::array<std::int64_t, 1>{1}, Format::Float32, true, index);
      if (!output) {
        f.error = output.error();
        return nil;
      }
      f.result = Buffer(*output);
    } else {
      auto readback = allocateMetal(state, index, 4, true);
      if (!readback) {
        f.error = readback.error();
        return nil;
      }
      f.staging = *readback;
    }
    id<MTLCommandBuffer> command =
        incoming ? incoming : (state->permit() ? [state->queues[index] commandBuffer] : nil);
    if (!command)
      return nil;
    std::uint64_t length = f.packet.input.bytes() / 4;
    // Empty device storage has no Metal allocation. The epilogue uses a
    // separately owned positive-zero seed rather than dereferencing that
    // storage.
    id<MTLBuffer> input = length ? f.packet.input.storage_->metal->buffer : nil;
    if (f.packet.epilogue) {
      // All fallible epilogue storage is prepared before opening any encoder.
      auto output = allocateMetal(state, index, 4, false);
      if (!output) {
        f.error = output.error();
        return nil;
      }
      f.scratch->postOutput = *output;
      const auto &bits = f.packet.scalarArguments->scalarBits;
      auto parameters = allocateMetal(state, index, (std::uint64_t(bits.size()) + 1) * 4, true);
      if (!parameters) {
        f.error = parameters.error();
        return nil;
      }
      f.scratch->postParameters = *parameters;
      auto *words = static_cast<std::uint32_t *>(f.scratch->postParameters->buffer.contents);
      words[0] = 1;
      std::copy(bits.begin(), bits.end(), words + 1);
      if (!length) {
        auto seed = allocateMetal(state, index, 4, true);
        if (!seed) {
          f.error = seed.error();
          return nil;
        }
        f.scratch->seed = *seed;
        std::uint32_t zero = 0;
        std::memcpy(f.scratch->seed->buffer.contents, &zero, 4);
        input = f.scratch->seed->buffer;
        length = 1;
      }
    }
    if (resident && !length) {
      auto seed = allocateMetal(state, index, 4, true);
      if (!seed) {
        f.error = seed.error();
        return nil;
      }
      f.scratch->seed = *seed;
      std::uint32_t zero = 0;
      std::memcpy(f.scratch->seed->buffer.contents, &zero, 4);
      input = f.scratch->seed->buffer;
      length = 1;
    }
    const bool packedCounts = [state->devices[index] supportsFamily:MTLGPUFamilyApple1];
    const auto blockPipeline = f.packet.kernel.state_->blockReduction;
    const std::uint64_t divisor = blockPipeline ? 128 : 2;
    // The final destination is already owned by this frame until command
    // acknowledgement. Write the last reduction directly into it, avoiding a
    // private scalar allocation and a separate GPU blit/encoder. Single-leaf
    // inputs still need a copy, and scalar epilogues keep their staged path.
    const bool directFinal = length > 1 && !f.packet.epilogue;
    id<MTLBuffer> finalOutput = resident ? f.result.storage_->metal->buffer : f.staging->buffer;
    unsigned levels = 0;
    for (auto remaining = length; remaining > 1; remaining = (remaining + divisor - 1) / divisor)
      ++levels;
    if (levels && packedCounts) {
      // Apple-family constant buffer offsets permit four-byte alignment. Each
      // dispatch receives a distinct immutable word, retained until
      // acknowledgement.
      auto count = allocateMetal(state, index, levels * 4, true);
      if (!count) {
        f.error = count.error();
        return nil;
      }
      f.scratch->counts[0] = *count;
    }
    unsigned level = 0;
    while (length > 1) {
      const auto next = (length + divisor - 1) / divisor;
      auto &output = f.scratch->outputs[level % 2];
      if (!output && !(directFinal && next == 1)) {
        auto allocated = allocateMetal(state, index, next * 4, false);
        if (!allocated) {
          f.error = allocated.error();
          return nil;
        }
        output = *allocated;
      }
      // Later levels only shrink. Never write into the input or the current
      // read buffer.
      if (!packedCounts) {
        auto count = allocateMetal(state, index, 4, true);
        if (!count) {
          f.error = count.error();
          return nil;
        }
        f.scratch->counts[level] = *count;
      }
      auto bits = std::uint32_t(length);
      auto count = f.scratch->counts[packedCounts ? 0 : level]->buffer;
      std::memcpy(static_cast<std::byte *>(count.contents) + (packedCounts ? level * 4 : 0), &bits,
                  4);
      length = next;
      ++level;
    }
    // Finish fallible storage preparation before opening an encoder. Every
    // early allocation failure can unwind without leaving an active encoder
    // behind.
    if (level) {
      id<MTLComputeCommandEncoder> encoder =
          state->permit() ? [command computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial]
                          : nil;
      if (!encoder)
        return nil;
      [encoder
          setComputePipelineState:blockPipeline ? blockPipeline : f.packet.kernel.state_->pipeline];
      length = f.packet.input.bytes() / 4;
      for (unsigned i = 0; i < level; ++i) {
        const auto next = (length + divisor - 1) / divisor;
        id<MTLBuffer> output =
            directFinal && next == 1 ? finalOutput : f.scratch->outputs[i % 2]->buffer;
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:f.scratch->counts[packedCounts ? 0 : i]->buffer
                    offset:packedCounts ? i * 4 : 0
                   atIndex:2];
        if (state->options.profile)
          ++state->dispatches;
        [encoder dispatchThreadgroups:MTLSizeMake(blockPipeline ? next : (next + 63) / 64, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        input = output;
        length = next;
      }
      [encoder endEncoding];
    }
    if (f.packet.epilogue) {
      id<MTLComputeCommandEncoder> encoder =
          state->permit() ? [command computeCommandEncoder] : nil;
      if (!encoder)
        return nil;
      [encoder setComputePipelineState:f.packet.epilogue.state_->pipeline];
      [encoder setBuffer:input offset:0 atIndex:0];
      [encoder setBuffer:f.scratch->postOutput->buffer offset:0 atIndex:1];
      [encoder setBuffer:f.scratch->postParameters->buffer offset:0 atIndex:2];
      if (state->options.profile)
        ++state->dispatches;
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [encoder endEncoding];
      input = f.scratch->postOutput->buffer;
    }
    // Only the final scalar crosses back to the host. No CPU arithmetic
    // fallback.
    if (!directFinal) {
      id<MTLBlitCommandEncoder> blit = state->permit() ? [command blitCommandEncoder] : nil;
      if (!blit)
        return nil;
      [blit copyFromBuffer:input sourceOffset:0 toBuffer:finalOutput destinationOffset:0 size:4];
      [blit endEncoding];
    }
    if (!state->permit())
      return nil;
    return command;
  }
  static void nextMatrixStage(Frame &f) {
    if (f.packet.matrixLeftMap) {
      f.matrixStage = 1;
      f.packet.kernel = std::move(f.packet.matrixLeftMap);
      f.packet.arguments = std::move(f.packet.leftArguments);
      f.packet.kind = Kind::Elementwise;
    } else if (f.packet.matrixRightMap) {
      f.matrixStage = 2;
      f.packet.kernel = std::move(f.packet.matrixRightMap);
      f.packet.arguments = std::move(f.packet.rightArguments);
      f.packet.kind = Kind::Elementwise;
    } else {
      f.matrixStage = 3;
      f.packet.kernel = std::move(f.matrixKernel);
      f.packet.input = std::move(f.matrixLeft);
      f.packet.right = std::move(f.matrixRight);
      f.packet.kind = Kind::Matmul;
    }
  }
  static void captureJoin(PacketData &packet, Kernel join,
                          std::span<const float> scalars) noexcept {
    if (!packet.graph)
      return;
    const auto &base = packet.kernel.state_, &reduction = packet.reduction.state_;
    const auto &kernel = join.state_;
    if (!kernel || !base || !reduction || !reduction->reduction || reduction->matrix ||
        !kernel->elementwise || kernel->reduction || kernel->matrix ||
        kernel->signature.value_or(ElementwiseSignature{2, 0}).tensors !=
            packet.graph->reduced.size()) {
      packet.preparation = Error::Invalid;
      return;
    }
    if (reduction->registry != packet.state || kernel->registry != packet.state ||
        reduction->index != base->index || kernel->index != base->index) {
      packet.preparation = Error::Residency;
      return;
    }
    auto args = capture(packet.state, join, packet.graph->reduced, scalars);
    if (!args) {
      packet.preparation = args.error();
      return;
    }
    packet.graph->join = std::move(join);
    packet.graph->joinArguments = *args;
  }
  static void captureTensorGraph(PacketData &packet, std::span<const Buffer> inputs,
                                 std::span<const MatrixProduct> products,
                                 std::span<const std::uint32_t> roots, std::span<const Kernel> maps,
                                 std::span<const float> scalars,
                                 std::span<const MatrixScalar> broadcasts) noexcept {
    const auto &base = packet.kernel.state_, &reduction = packet.reduction.state_;
    if (!base || !reduction || !reduction->reduction || reduction->matrix) {
      packet.preparation = Error::Invalid;
      return;
    }
    if (reduction->registry != packet.state || reduction->index != base->index) {
      packet.preparation = Error::Residency;
      return;
    }
    auto graph = captureGraph(packet.state, packet.kernel, inputs, products, maps, scalars, roots,
                              broadcasts, true);
    if (graph)
      packet.graph = *graph;
    else
      packet.preparation = graph.error();
  }
  static void nextGraphStage(Frame &f) {
    const auto &graph = *f.packet.graph;
    if (!f.graphKernel)
      f.graphKernel = f.packet.kernel;
    if (!graph.reductions.empty()) {
      if (f.reductionStage < graph.reductions.size() &&
          graph.reductions[f.reductionStage] < graph.inputs + f.graphStage)
        f.graphPhase = 1;
      else if (f.graphStage == graph.products.size())
        f.graphPhase = 2;
      if (f.graphPhase == 1) {
        f.packet.kind = Kind::Reduce;
        f.packet.kernel = f.packet.reduction;
        f.packet.input = graph.values[graph.reductions[f.reductionStage]];
        f.packet.right = {};
        f.packet.arguments.reset();
        return;
      }
      if (f.graphPhase == 2) {
        f.packet.kind = Kind::Elementwise;
        f.packet.kernel = graph.join;
        f.packet.arguments = graph.joinArguments;
        f.packet.input = graph.reduced.front();
        f.packet.right = {};
        return;
      }
    }
    const auto edge = graph.products[f.graphStage];
    const bool map = !graph.maps.empty() && bool(graph.maps[f.graphStage]);
    f.packet.kernel = map ? graph.maps[f.graphStage] : f.graphKernel;
    f.packet.kind = map ? Kind::Elementwise : Kind::Matmul;
    f.packet.arguments = map ? graph.mapArguments[f.graphStage] : nullptr;
    f.packet.input = graph.values[edge.left];
    f.packet.right = graph.values[edge.right];
  }
  static void releaseGraphOperands(MatrixGraphArguments &graph, std::size_t stage) {
    const auto &edge = graph.products[stage];
    const auto release = [&](std::uint32_t operand) {
      if (!--graph.uses[operand])
        graph.values[operand] = {};
    };
    if (edge.operands.empty()) {
      release(edge.left);
      release(edge.right);
    } else
      for (auto operand : edge.operands)
        release(operand);
  }
  // This is a dispatch policy, not a language bound. Large operations retain
  // their individual acknowledgement checkpoints. Count products for matrices
  // and output elements for maps; neither count is a wall-clock guarantee.
  static constexpr std::uint64_t graphWorkLimit = 64 * 1024 * 1024;
  static std::uint64_t graphStageWork(const Frame &f, std::size_t stage) {
    const auto &graph = *f.packet.graph;
    const auto &shape = graph.shapes[graph.inputs + stage];
    const auto cells = std::uint64_t(shape[0]) * std::uint64_t(shape[1]);
    if (!cells || cells > graphWorkLimit)
      return graphWorkLimit + 1;
    if (!graph.maps.empty() && graph.maps[stage])
      return cells;
    const auto inner = std::uint64_t(graph.shapes[graph.products[stage].left][1]);
    return inner > graphWorkLimit / cells ? graphWorkLimit + 1 : cells * inner;
  }
  static bool batchGraphStage(const Frame &f) {
    if (!f.packet.graph || f.graphPhase || f.packet.numericProgram ||
        !f.packet.graph->reductions.empty() || !f.graphKernel.state_->tiledMatrix ||
        f.packet.kernel.state_->numericType || f.graphBatched == f.graphParameters.size() ||
        f.graphStage + 1 == f.packet.graph->products.size() || !f.result.bytes())
      return false;
    const auto &graph = *f.packet.graph;
    const auto next = f.graphStage + 1;
    const auto &kernel =
        !graph.maps.empty() && graph.maps[next] ? graph.maps[next].state_ : f.graphKernel.state_;
    if (kernel->numericType)
      return false;
    const auto work = graphStageWork(f, f.graphStage), following = graphStageWork(f, next);
    if (work > graphWorkLimit || f.graphWork > graphWorkLimit - work ||
        following > graphWorkLimit - work - f.graphWork)
      return false;
    const auto &shape = graph.shapes[graph.inputs + next];
    const auto bytes = std::uint64_t(shape[0]) * std::uint64_t(shape[1]) * 4;
    if (!bytes)
      return false; // Empty stages keep their no-command path.
    const auto parameters = kernel->matrix ? 12 : 4ull * kernel->parameterWords();
    const auto needed = bytes + parameters + kernel->encodedBytes;
    const auto &state = f.packet.state;
    const auto used = state->bytes.load();
    // Flush before the next allocation when batching would prevent last-use
    // reclamation from fitting it. The normal allocator remains authoritative
    // if another task consumes the available budget concurrently.
    return used <= state->options.maxBytes && needed <= state->options.maxBytes - used;
  }
  static bool batchGraphReduction(const Frame &f) {
    if (!f.packet.graph || f.graphPhase || f.packet.numericProgram ||
        !f.packet.graph->reductions.empty() || f.packet.graph->tensorResult ||
        !f.packet.reduction.state_ || !f.packet.reduction.state_->blockReduction ||
        f.packet.kernel.state_->numericType || f.graphBatched > 2 ||
        f.graphStage + 1 != f.packet.graph->products.size() || !f.result.bytes())
      return false;
    // Include the reduction as a logical stage. Its full tree requires fewer
    // than twice as many element operations as leaves. No work/cancellation
    // allowance is enlarged to remove this submission boundary.
    const auto cells = f.result.bytes() / 4;
    const auto work = graphStageWork(f, f.graphStage);
    if (cells > graphWorkLimit / 2 || work > graphWorkLimit - 2 * cells ||
        f.graphWork > graphWorkLimit - 2 * cells - work)
      return false;
    const auto used = f.packet.state->bytes.load();
    const auto maximum = f.packet.state->options.maxBytes;
    // Preserve the acknowledged path for tight budgets, which may require
    // reclaiming the graph's last-use operands before reduction preparation.
    return used <= maximum && 4 * f.result.bytes() + 4096 <= maximum - used;
  }
  static Buffer reusableGraphMapResult(const Frame &f) {
    if (!f.packet.graph || f.graphPhase || f.packet.numericProgram ||
        !f.packet.graph->reductions.empty() || f.packet.kind != Kind::Elementwise ||
        f.packet.kernel.state_->numericType || f.packet.kernel.state_->projected ||
        f.packet.kernel.state_->indirect())
      return {};
    const auto &graph = *f.packet.graph;
    const auto &edge = graph.products[f.graphStage];
    const auto candidate = [&](std::uint32_t value) -> Buffer {
      // Original arguments remain immutable even if their backing happens to
      // have no external alias. Only graph-private, single-consumer results
      // with same-cell maps can safely donate their output storage.
      if (value < graph.inputs)
        return {};
      const auto &buffer = graph.values[value];
      const auto occurrences =
          edge.operands.empty() ? unsigned(edge.left == value) + unsigned(edge.right == value) : 1u;
      if (graph.uses[value] != occurrences || !buffer.onDevice() ||
          buffer.format() != Format::Float32 || !buffer.bytes() || buffer.shape().size() != 2 ||
          f.packet.input.shape().size() != 2 || buffer.bytes() != f.packet.input.bytes() ||
          !std::equal(buffer.shape().begin(), buffer.shape().end(), f.packet.input.shape().begin()))
        return {};
      return buffer;
    };
    if (edge.operands.empty()) {
      if (auto result = candidate(edge.left))
        return result;
      return candidate(edge.right);
    }
    for (auto value : edge.operands)
      if (auto result = candidate(value))
        return result;
    return {};
  }
  static id<MTLCommandBuffer> tensorTransformCommand(Frame &f) {
    const auto &kernel = *f.packet.kernel.state_;
    const auto &source = f.packet.input.storage_;
    const auto state = f.packet.state;
    f.error = Error::Invalid;
    if (!kernel.transform || !source || source->rank != 2 || f.packet.numericScalar ||
        std::uint64_t(source->dimensions[0]) > UINT32_MAX ||
        std::uint64_t(source->dimensions[1]) > UINT32_MAX)
      return nil;
    const auto kind = *kernel.transform;
    auto shape = std::array{source->dimensions[0], source->dimensions[1]};
    if (kind == TensorTransformKind::Transpose)
      std::swap(shape[0], shape[1]);
    auto output = allocateStorage(state, shape, source->format, true, kernel.index);
    if (!output) {
      f.error = output.error();
      return nil;
    }
    f.result = Buffer(*output);
    if (!(*output)->metal) {
      auto dummy = allocateMetal(state, kernel.index, 4, false);
      if (!dummy) {
        f.error = dummy.error();
        return nil;
      }
      (*output)->metal = *dummy;
    }
    auto parameters = allocateMetal(state, kernel.index, 24, true);
    if (!parameters) {
      f.error = parameters.error();
      return nil;
    }
    f.staging = *parameters;
    auto *words = static_cast<std::uint32_t *>(f.staging->buffer.contents);
    const auto count = source->size / describeNumeric(*kernel.numericType)->bytes();
    words[0] = std::uint32_t(count);
    words[1] = std::uint32_t(source->dimensions[0]);
    words[2] = std::uint32_t(source->dimensions[1]);
    words[3] =
        f.packet.numericProgram && !f.packet.numericProgram->captures.empty() &&
                !f.packet.numericProgram->code[f.packet.numericProgram->pc].scalars.empty()
            ? std::uint32_t(
                  f.packet.numericProgram->captures
                      [f.packet.numericProgram->code[f.packet.numericProgram->pc].scalarOffset])
            : (f.packet.arguments->scalarBits.empty()
                   ? 0
                   : std::uint32_t(f.packet.arguments->scalarBits[0]));
    words[4] = kernel.signature->tensors == 2;
    words[5] = UINT32_MAX;
    id<MTLBuffer> scratch = (*output)->metal->buffer;
    if (kind != TensorTransformKind::Transpose) {
      auto temporary =
          allocateMetal(state, kernel.index, std::max(std::uint64_t(4), source->size), false);
      if (!temporary) {
        f.error = temporary.error();
        return nil;
      }
      f.transformScratch = *temporary;
      scratch = f.transformScratch->buffer;
    }
    id<MTLBuffer> input = source->metal ? source->metal->buffer : (*output)->metal->buffer;
    id<MTLBuffer> epsilon = words[4] ? tensor(f, 1).storage_->metal->buffer : input;
    f.error = Error::Allocation;
    auto command = f.pendingCommand;
    if (!command)
      command = state->permit() ? [state->queues[kernel.index] commandBuffer] : nil;
    auto encoder = command && state->permit() ? [command computeCommandEncoder] : nil;
    if (!encoder)
      return nil;
    [encoder setComputePipelineState:kernel.pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:epsilon offset:0 atIndex:1];
    [encoder setBuffer:(*output)->metal->buffer offset:0 atIndex:2];
    [encoder setBuffer:scratch offset:0 atIndex:3];
    [encoder setBuffer:f.staging->buffer offset:0 atIndex:4];
    const auto groups = kind == TensorTransformKind::Transpose
                            ? std::max(UINT64_C(1), (count + 255) / 256)
                        : count ? std::uint64_t(words[1])
                                : UINT64_C(1);
    if (state->options.profile)
      ++state->dispatches;
    [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [encoder endEncoding];
    if (!state->permit())
      return nil;
    return command;
  }
  static void submitCommand(Task &task, Frame &f, id<MTLCommandBuffer> command) {
    auto state = f.packet.state;
    f.started = true;
    f.pendingCommand = nil;
    f.error = Error::Device;
    auto completion = task.suspendExternal();
    if (!completion)
      return;
    auto *frame = &f;
    [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
#if __has_feature(thread_sanitizer)
      // Metal is not instrumented. Its commit-to-completion handoff also
      // publishes the copied block and the leased frame. Use the callback
      // argument as the per-command token, before reading any captures.
      __tsan_acquire((__bridge void *)finished);
#endif
      @autoreleasepool {
        // The external lease pins frame. Never access it after signal().
        {
          std::unique_lock lock(frame->packet.state->gateMutex);
          frame->packet.state->gate.wait(lock, [&] { return !frame->packet.state->hold; });
        }
        if (frame->packet.state->options.profile && finished.GPUEndTime >= finished.GPUStartTime)
          frame->packet.state->gpuNanoseconds +=
              std::uint64_t((finished.GPUEndTime - finished.GPUStartTime) * 1e9);
        frame->succeeded = finished.status == MTLCommandBufferStatusCompleted &&
                           !frame->packet.state->failCompletion.exchange(false);
        ++frame->packet.state->completions;
        --frame->packet.state->inFlight;
        frame->active = false;
        completion.signal();
      }
    }];
    ++state->submissions;
#if __has_feature(thread_sanitizer)
    __tsan_release((__bridge void *)command);
#endif
    [command commit];
  }
  static void flushNumeric(Task &task, Frame &f) {
    f.numericAcknowledging = true;
    submitCommand(task, f, f.pendingCommand);
  }
  static void fail(Task &task, Frame &f, std::int64_t error) {
    if (f.numericBatched && !f.numericAcknowledging) {
      f.numericDeferredError = error;
      flushNumeric(task, f);
    } else
      task.fault(error);
  }
  static std::uint64_t numericScalar(const NumericProgramArguments &program,
                                     const NumericProgramArguments::Instruction &op,
                                     std::size_t i) {
    return program.captures.empty() ? op.scalars[i] : program.captures[op.scalarOffset + i];
  }
  // Non-faulting arithmetic is a compiler proof; shapes, registry, residency,
  // domain and RMS epsilon are still checked from this invocation's bindings.
  static bool numericPostCandidate(const Frame &f) {
    const auto &p = *f.packet.numericProgram;
    const auto &op = p.code[p.pc];
    if (!p.definition || op.kind != NumericInstructionKind::Matmul ||
        op.kernel.state_->numericType != NumericType::Float32 || p.pc + 1 >= p.code.size() ||
        p.uses[op.output] != 1 || f.numericBatched > 2)
      return false;
    const auto &post = p.code[p.pc + 1];
    const auto &k = post.kernel.state_;
    if (post.kind != NumericInstructionKind::Map || post.target != 1 || !k || !post.arguments ||
        k->numericType != NumericType::Float32 || k->transform || k->indirect() ||
        k->readCount != 1 || k->reads[0] != 0 || post.inputs.size() != 1 ||
        post.inputs[0] != op.output || p.values[post.output] ||
        std::none_of(k->fusedMatrices.begin(), k->fusedMatrices.end(),
                     [](auto x) { return x != nil; }))
      return false;
    const auto &a = p.values[op.inputs[0]], &b = p.values[op.inputs[1]];
    if (!a.bytes() || !b.bytes() || a.format() != Format::Float32 ||
        b.format() != Format::Float32 || a.shape().size() != 2 || b.shape().size() != 2 ||
        a.shape()[1] != b.shape()[0])
      return false;
    const auto cells = std::uint64_t(a.shape()[0]) * b.shape()[1];
    const auto used = p.state->bytes.load(), maximum = p.state->options.maxBytes;
    return cells <= UINT32_MAX && used <= maximum && cells * 12 <= maximum - used &&
           maximum - used - cells * 12 >= 64 * 1024 + k->parameterWords() * 4;
  }
  static void finishNumeric(Frame &f) {
    auto &p = *f.packet.numericProgram;
    const auto &op = p.code[p.pc];
    const auto &last = f.numericFused ? p.code[p.pc + 1] : op;
    p.values[last.output] = std::move(f.result);
    if (!p.origins.empty())
      p.origins[last.output] =
          f.numericResultOrigin == UINT32_MAX ? last.output : f.numericResultOrigin;
    for (auto input : op.inputs)
      p.release(input);
    if (f.numericFused)
      for (auto input : last.inputs)
        p.release(input);
    p.pc += f.numericFused ? 2 : 1;
  }
  static Buffer reusableNumericMapResult(Frame &f) {
    if (!f.packet.numericProgram || !f.packet.numericProgram->definition ||
        f.packet.kind != Kind::Elementwise ||
        f.packet.numericProgram->pc >= f.packet.numericProgram->code.size())
      return {};
    auto &p = *f.packet.numericProgram;
    const auto &op = p.code[p.pc];
    const auto &k = op.kernel.state_;
    if (op.kind != NumericInstructionKind::Map || op.target != 1 || !k || k->transform ||
        k->indirect() || k->numericType != NumericType::Float32 || !k->readCount ||
        std::any_of(k->scalarBuffers.begin(), k->scalarBuffers.end(),
                    [](auto role) { return role; }))
      return {};
    const auto &domain = tensor(f, k->tensorIndex(0));
    for (auto id : op.inputs) {
      const auto &value = p.values[id];
      if (id == op.output || p.origins[id] == UINT32_MAX || !value.bytes() ||
          value.format() != Format::Float32 || value.shape().size() != domain.shape().size() ||
          !std::equal(value.shape().begin(), value.shape().end(), domain.shape().begin()) ||
          p.uses[id] != std::count(op.inputs.begin(), op.inputs.end(), id))
        continue;
      bool alias = false;
      for (std::size_t i = 0; i < p.values.size(); ++i)
        if (i != id && p.uses[i] && p.values[i].storage_ == value.storage_) {
          alias = true;
          break;
        }
      if (!alias) {
        f.numericResultOrigin = p.origins[id];
        return value;
      }
    }
    return {};
  }
  static std::optional<std::uint64_t> numericBatchWork(const Frame &f) {
    constexpr std::uint64_t limit = UINT64_C(64) * 1024 * 1024;
    const auto &p = *f.packet.numericProgram;
    const auto &op = p.code[p.pc];
    const auto &k = op.kernel.state_;
    using K = NumericInstructionKind;
    if (!p.definition || !k || k->numericType != NumericType::Float32 ||
        (op.kind != K::Map && op.kind != K::Matmul) || op.inputs.empty() ||
        bool(p.values[op.output]))
      return {};
    for (auto value : op.inputs) {
      const auto &v = p.values[value];
      if (!v || !v.onDevice() || !v.bytes() || v.format() != Format::Float32 ||
          v.storage_->state != p.state || v.storage_->deviceIndex != p.index ||
          v.bytes() / 4 > UINT32_MAX)
        return {};
      for (auto n : v.shape())
        if (std::uint64_t(n) > UINT32_MAX)
          return {};
    }
    const auto &a = p.values[op.inputs[0]];
    if (op.kind == K::Matmul) {
      const auto &b = p.values[op.inputs[1]];
      if (a.shape().size() != 2 || b.shape().size() != 2 || a.shape()[1] != b.shape()[0])
        return {};
      const auto cells = std::uint64_t(a.shape()[0]) * b.shape()[1];
      const auto inner = std::uint64_t(a.shape()[1]);
      if (cells > UINT32_MAX)
        return {};
      const auto matrixWork = inner && cells > limit / 2 / inner ? limit + 1 : cells * inner * 2;
      return std::min(limit + 1, matrixWork + (f.numericPostWanted ? cells * 2 : 0));
    }
    if (k->indirect() || k->readCount == 0 ||
        std::any_of(k->scalarBuffers.begin(), k->scalarBuffers.end(), [](auto x) { return x; }) ||
        std::any_of(k->scalarTypes.begin(), k->scalarTypes.end(),
                    [](auto t) { return t != NumericType::Float32; }))
      return {};
    for (auto value : op.inputs) {
      const auto shape = p.values[value].shape();
      if (!std::equal(shape.begin(), shape.end(), a.shape().begin(), a.shape().end()))
        return {};
    }
    if (k->transform) {
      if (a.shape().size() != 2)
        return {};
      if (*k->transform == TensorTransformKind::RmsNormRows) {
        if (op.scalars.size() != 1 || op.inputs.size() != 1)
          return {};
        const auto epsilon = std::uint32_t(numericScalar(p, op, 0));
        if (!epsilon || (epsilon >> 31) || (epsilon & 0x7f800000u) == 0x7f800000u)
          return {};
      }
      return std::min(limit + 1, a.bytes() / 4 * 8);
    }
    if (op.target != 1)
      return {};
    return std::min(limit + 1, a.bytes() / 4 * 2);
  }
  static bool numericHeadroom(const Frame &f, std::uint64_t work) {
    constexpr auto limit = UINT64_C(64) * 1024 * 1024;
    if (work > limit || f.numericWork > limit - work)
      return false;
    const auto &p = *f.packet.numericProgram;
    const auto &op = p.code[p.pc];
    const auto &a = p.values[op.inputs[0]];
    const auto output = op.kind == NumericInstructionKind::Matmul
                            ? std::uint64_t(a.shape()[0]) * p.values[op.inputs[1]].shape()[1] * 4
                            : a.bytes();
    const auto used = p.state->bytes.load();
    const auto maximum = p.state->options.maxBytes;
    // Preserve the individual-stage reclamation path under tight budgets.
    const auto parameters = op.kernel.state_->parameterWords() * 4;
    return used <= maximum && output <= (maximum - used) / 3 &&
           parameters <= (maximum - used - output * 3) / 3 &&
           maximum - used - output * 3 - parameters * 3 >= 64 * 1024;
  }
  static void resume(Task &task) {
    for (;;) {
      @autoreleasepool {
        auto &f = *static_cast<Frame *>(task.frame());
        auto state = f.packet.state;
        if (f.packet.numericProgram && !f.packet.preparation) {
          auto &program = *f.packet.numericProgram;
          if (f.started) {
            if (!f.succeeded) {
              fail(task, f, std::int64_t(f.error));
              return;
            }
            if (f.numericAcknowledging) {
              for (unsigned i = 0; i < f.numericBatched; ++i)
                if (static_cast<const std::uint32_t *>(
                        f.numericParameters[i]->buffer.contents)[f.numericFlagWords[i]] !=
                    UINT32_MAX) {
                  fail(task, f, 4);
                  return;
                }
              f.numericParameters = {};
              f.numericResources = {};
              f.numericScratch = {};
              f.numericBatched = 0;
              f.numericWork = 0;
              f.numericAcknowledging = false;
              program.deferRetirement = false;
              for (auto value : program.retired)
                program.values[value] = {};
              program.retired.clear();
              if (f.numericDeferredError) {
                fail(task, f, *f.numericDeferredError);
                return;
              }
            } else if (program.reading || program.condition) {
              auto bytes = static_cast<const std::byte *>(f.staging->buffer.contents);
              if (program.reading) {
                completeNumeric(task, program.element, bytes);
                return;
              }
              const auto value = std::to_integer<unsigned>(*bytes);
              if (value > 1) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              const auto &op = program.code[program.pc];
              for (auto input : op.inputs)
                program.release(input);
              if (value)
                ++program.pc;
              else {
                program.skip(program.pc + 1, op.target);
                program.pc = op.target;
              }
              program.condition = false;
            } else {
              const auto &kernel = f.packet.kernel.state_;
              if (f.packet.kind == Kind::Reduce) {
                for (const auto &parameters : f.scratch->counts)
                  if (parameters && static_cast<const std::uint32_t *>(
                                        parameters->buffer.contents)[1] != UINT32_MAX) {
                    fail(task, f, 4);
                    return;
                  }
              } else if (kernel && kernel->numericType && f.staging &&
                         static_cast<const std::uint32_t *>(
                             f.staging->buffer.contents)[kernel->parameterWords() - 1] !=
                             UINT32_MAX) {
                fail(task, f, 4);
                return;
              }
              if (f.numericFused && f.fusedParameters &&
                  static_cast<const std::uint32_t *>(
                      f.fusedParameters->buffer
                          .contents)[program.code[program.pc + 1].kernel.state_->parameterWords() -
                                     1] != UINT32_MAX) {
                fail(task, f, 4);
                return;
              }
              finishNumeric(f);
            }
            f.staging.reset();
            f.fusedParameters.reset();
            f.fusedPostKernel = {};
            f.resources.reset();
            f.transformScratch.reset();
            f.scratch.reset();
            f.result = {};
            f.packet.input = {};
            f.packet.right = {};
            f.packet.kernel = {};
            f.packet.arguments.reset();
            f.started = f.succeeded = false;
            if (task.cancelled())
              return;
          }
          bool scheduled = false;
          while (!scheduled) {
            if (task.cancelled())
              return;
            if (f.numericBatched == 4 || (f.numericBatched && program.pc == program.code.size())) {
              flushNumeric(task, f);
              return;
            }
            if (program.pc == program.code.size()) {
              const auto &value = program.values[program.result];
              if (!value || value.format() != *storageFormat(program.element)) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              if (!program.scalar) {
                task.complete(&value);
                return;
              }
              if (value.bytes() != describeNumeric(program.element)->bytes()) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              f.packet.input = value;
              f.packet.kind = Kind::Download;
              program.reading = true;
              scheduled = true;
              break;
            }
            const auto &op = program.code[program.pc];
            bool valid = true;
            for (auto value : op.inputs)
              valid &= bool(program.values[value]);
            if (!valid) {
              fail(task, f, std::int64_t(Error::Invalid));
              return;
            }
            auto input = [&](std::size_t i) -> const Buffer & {
              return program.values[op.inputs[i]];
            };
            using K = NumericInstructionKind;
            f.numericEligible = false;
            f.numericFused = false;
            f.numericResultOrigin = UINT32_MAX;
            f.numericPostWanted = numericPostCandidate(f);
            if (op.kind == K::Map || op.kind == K::Matmul || op.kind == K::Reduce ||
                op.kind == K::Branch || (op.kind == K::Alias && program.values[op.output])) {
              const auto work = numericBatchWork(f);
              f.numericEligible = work && numericHeadroom(f, *work);
              if (f.numericBatched && !f.numericEligible) {
                flushNumeric(task, f);
                return;
              }
            }
            if (op.kind == K::Jump) {
              program.skip(program.pc + 1, op.target);
              program.pc = op.target;
              continue;
            }
            if (op.kind == K::ShapeFault) {
              fail(task, f, 1);
              return;
            }
            if (op.kind == K::Alias) {
              if (op.inputs.size() != 1) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              program.values[op.output] = input(0);
              if (!program.origins.empty())
                program.origins[op.output] = program.origins[op.inputs[0]];
              program.release(op.inputs[0]);
              ++program.pc;
              continue;
            }
            if (op.kind == K::EqualShape || op.kind == K::MatrixShape || op.kind == K::Extent) {
              bool same = false;
              if (op.kind == K::Extent && op.inputs.size() == 1 && op.scalars.size() % 2 == 0) {
                same = true;
                for (std::size_t i = 0; i < op.scalars.size(); i += 2)
                  same &= op.scalars[i] < input(0).shape().size() &&
                          std::uint64_t(input(0).shape()[op.scalars[i]]) == op.scalars[i + 1];
              } else if (op.kind == K::Extent && op.inputs.size() == 2 && op.scalars.size() == 2) {
                const auto a = input(0).shape(), b = input(1).shape();
                same = op.scalars[0] < a.size() && op.scalars[1] < b.size() &&
                       a[op.scalars[0]] == b[op.scalars[1]];
              } else if (op.inputs.size() == 2) {
                const auto a = input(0).shape(), b = input(1).shape();
                same = op.kind == K::MatrixShape
                           ? a.size() == 2 && b.size() == 2 && a[1] == b[0]
                           : std::equal(a.begin(), a.end(), b.begin(), b.end());
              }
              if (!same) {
                fail(task, f, 1);
                return;
              }
              for (auto value : op.inputs)
                program.release(value);
              ++program.pc;
              continue;
            }
            if (op.kind == K::Branch) {
              if (op.inputs.size() != 1 || input(0).format() != Format::UInt8 ||
                  input(0).bytes() != 1) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              f.packet.input = input(0);
              f.packet.kind = Kind::Download;
              program.condition = true;
              scheduled = true;
              break;
            }
            const auto &kernel = op.kernel.state_;
            if (!kernel || !kernel->numericType) {
              fail(task, f, std::int64_t(Error::Invalid));
              return;
            }
            f.packet.kernel = op.kernel;
            if (op.kind == K::Map) {
              if (!kernel->elementwise || kernel->matrix || kernel->reduction) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              if (op.arguments) {
                f.packet.arguments = op.arguments;
              } else {
                std::vector<Buffer> placeholders;
                try {
                  if (!state->permit()) {
                    fail(task, f, 2);
                    return;
                  }
                  placeholders.resize(op.inputs.size());
                } catch (...) {
                  fail(task, f, 2);
                  return;
                }
                auto arguments = captureNumeric(state, op.kernel, placeholders, op.scalars);
                if (!arguments) {
                  fail(task, f, std::int64_t(arguments.error()));
                  return;
                }
                f.packet.arguments = *arguments;
              }
              f.packet.kind = Kind::Elementwise;
              if (!kernel->readCount) {
                auto value = allocateStorage(state, {}, *storageFormat(*kernel->numericType), true,
                                             kernel->index);
                if (!value) {
                  fail(task, f, std::int64_t(value.error()));
                  return;
                }
                f.packet.input = Buffer(*value);
              }
            } else if (op.kind == K::Reduce && op.inputs.size() == 1 && kernel->reduction) {
              f.packet.kind = Kind::Reduce;
              f.packet.input = input(0);
            } else if (op.kind == K::Matmul && op.inputs.size() == 2 && kernel->matrix) {
              f.packet.kind = Kind::Matmul;
              f.packet.input = input(0);
              f.packet.right = input(1);
            } else {
              fail(task, f, std::int64_t(Error::Invalid));
              return;
            }
            scheduled = true;
          }
        }
        if (!f.started && !f.matrixStage && (f.packet.matrixLeftMap || f.packet.matrixRightMap) &&
            !f.packet.preparation) {
          // Keep both original operands until their independent maps
          // acknowledge.
          f.matrixLeft = std::move(f.packet.input);
          f.matrixRight = std::move(f.packet.right);
          f.matrixKernel = std::move(f.packet.kernel);
          nextMatrixStage(f);
        }
        if (f.packet.graph && !f.started && !f.packet.preparation) {
          nextGraphStage(f);
        }
        if (f.started) {
          f.transformScratch.reset();
          f.fusedPostKernel = {};
          f.fusedParameters.reset();
          f.batchedLeft = {};
          f.batchedRight = {};
          f.batchedKernel = {};
          f.batchedParameters.reset();
          if (!f.succeeded) {
            fail(task, f, static_cast<std::int64_t>(f.error));
            return;
          }
          if (f.packet.kind != Kind::Reduce && f.packet.kernel.state_ &&
              f.packet.kernel.state_->numericType && f.staging) {
            const auto words = f.packet.kernel.state_->parameterWords();
            auto parameters = static_cast<const std::uint32_t *>(f.staging->buffer.contents);
            if (parameters[words - 1] != UINT32_MAX) {
              fail(task, f, 4);
              return;
            }
          }
          if (f.packet.numericScalar) {
            completeNumeric(task, *f.packet.kernel.state_->numericType,
                            f.joinReadback->buffer.contents);
            return;
          }
          if (f.packet.kind == Kind::Reduce && f.packet.kernel.state_ &&
              f.packet.kernel.state_->numericType) {
            for (const auto &parameters : f.scratch->counts) {
              if (parameters && static_cast<const std::uint32_t *>(
                                    parameters->buffer.contents)[1] != UINT32_MAX) {
                fail(task, f, 4);
                return;
              }
            }
            completeNumeric(task, *f.packet.kernel.state_->numericType, f.staging->buffer.contents);
            return;
          }
          if (f.packet.graph) {
            auto &graph = *f.packet.graph;
            // Earlier encoded results were private to this task and their
            // last-use reclamation was deferred until actual acknowledgement.
            for (std::size_t i = f.graphStage - f.graphBatched; i < f.graphStage; ++i)
              releaseGraphOperands(graph, i);
            f.graphParameters = {};
            f.graphResources = {};
            f.graphPostParameters = {};
            f.graphBatched = 0;
            f.graphWork = 0;
            if (!graph.reductions.empty() && f.graphPhase > 0) {
              if (f.graphPhase == 2) {
                std::memcpy(&f.scalar, f.joinReadback->buffer.contents, 4);
                f.packet.arguments.reset();
                f.packet.input = {};
                f.packet.right = {};
                f.packet.graph.reset();
                f.result = {};
                f.scratch.reset();
                f.staging.reset();
                f.resources.reset();
                f.joinReadback.reset();
                task.complete(&f.scalar);
                return;
              }
              f.staging.reset();
              f.scratch.reset();
              f.packet.input = {};
              f.packet.right = {};
              graph.reduced[f.reductionStage] = std::move(f.result);
              const auto root = graph.reductions[f.reductionStage];
              if (!--graph.uses[root])
                graph.values[root] = {};
              ++f.reductionStage;
              f.graphPhase = 0;
              f.started = f.succeeded = false;
              if (task.cancelled())
                return;
              continue;
            }
            f.staging.reset();
            f.resources.reset();
            f.packet.input = {};
            f.packet.right = {};
            f.packet.arguments.reset();
            graph.values[graph.inputs + f.graphStage] = std::move(f.result);
            releaseGraphOperands(graph, f.graphStage);
            ++f.graphStage;
            if (graph.tensorResult && f.graphStage == graph.products.size()) {
              f.result = std::move(graph.values.back());
              f.packet.graph.reset();
              f.packet.reduction = {};
              f.graphKernel = {};
              task.complete(&f.result);
              return;
            }
            if (!graph.reductions.empty()) {
              f.started = f.succeeded = false;
              if (task.cancelled())
                return;
              continue;
            }
            if (f.graphStage == graph.products.size()) {
              f.result = std::move(graph.values.back());
              f.packet.graph.reset();
              f.graphKernel = {};
              if (f.packet.reduction) {
                if (task.cancelled())
                  return;
                f.packet.input = std::move(f.result);
                f.packet.kernel = std::move(f.packet.reduction);
                f.packet.kind = Kind::Reduce;
                f.started = f.succeeded = false;
                continue;
              }
              task.complete(&f.result);
              return;
            }
            if (task.cancelled())
              return;
            f.started = f.succeeded = false;
            nextGraphStage(f);
          } else {
            // Materialize host results on the scheduler, never in the Metal
            // callback.
            if (f.packet.kind == Kind::Download) {
              auto *destination = const_cast<Storage *>(f.result.storage_.get());
              std::memcpy(destination->host.data(), f.staging->buffer.contents, destination->size);
            }
            if (f.packet.kind == Kind::Reduce)
              std::memcpy(&f.scalar, f.staging->buffer.contents, 4);
            f.staging.reset();
            f.resources.reset();
            f.scratch.reset();
            f.packet.input = Buffer{};
            f.packet.right = Buffer{};
            f.packet.kernel = Kernel{};
            f.packet.arguments.reset();
            f.packet.replacement = Buffer{};
            const bool beforeMatrix = f.matrixStage == 1 || f.matrixStage == 2;
            if (beforeMatrix) {
              (f.matrixStage == 1 ? f.matrixLeft : f.matrixRight) = std::move(f.result);
              nextMatrixStage(f);
            } else if (!f.packet.reduction && !f.packet.matrixPost) {
              f.packet.epilogue = Kernel{};
              f.packet.postArguments.reset();
              f.packet.scalarArguments.reset();
              task.complete(f.packet.kind == Kind::Reduce ? static_cast<const void *>(&f.scalar)
                                                          : &f.result);
              return;
            }
            // Acknowledgement transfers the resident result into the next
            // stage. Source argument packs remain immutable; slot zero may
            // select this result.
            if (beforeMatrix) {
              // nextMatrixStage has selected the next resident operand or
              // primitive.
            } else if (f.packet.matrixPost) {
              f.packet.replacement = std::move(f.result);
              f.packet.kernel = std::move(f.packet.matrixPost);
              f.packet.arguments = std::move(f.packet.postArguments);
              f.packet.kind = Kind::Elementwise;
            } else {
              f.packet.input = std::move(f.result);
              f.packet.kernel = std::move(f.packet.reduction);
              f.packet.kind = Kind::Reduce;
            }
            f.started = false;
            f.succeeded = false;
            if (task.cancelled())
              return;
          }
        }
        f.started = true;
        if (f.packet.preparation) {
          fail(task, f, std::int64_t(*f.packet.preparation));
          return;
        }
        if (f.packet.matrixPost && f.packet.kind == Kind::Matmul) {
          const auto &kernel = f.packet.matrixPost.state_;
          const auto &base = f.packet.kernel.state_;
          if (!base || !base->matrix || kernel->registry != state || kernel->index != base->index ||
              !f.packet.postArguments || !kernel->elementwise || kernel->matrix ||
              kernel->reduction || kernel->tensorIndex(0) != 0) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
        }
        if (f.packet.epilogue) {
          const auto &kernel = f.packet.epilogue.state_;
          const auto &base = f.packet.kernel.state_;
          const auto signature = kernel->signature.value_or(ElementwiseSignature{2, 0});
          if (!base || !kernel->elementwise || kernel->reduction || kernel->matrix ||
              kernel->projected || kernel->indirect() || signature.tensors != 1 ||
              !f.packet.scalarArguments ||
              signature.scalars != f.packet.scalarArguments->scalarBits.size() ||
              kernel->registry != state || kernel->index != base->index) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
        }
        if (f.packet.reduction) {
          const auto &reduction = f.packet.reduction.state_;
          const auto &mapping = f.packet.kernel.state_;
          if (!reduction->reduction || reduction->matrix || !mapping ||
              reduction->registry != state || reduction->index != mapping->index) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
        }
        if (f.packet.arguments && !f.packet.numericScalar && f.packet.kernel.state_->readCount) {
          const auto &kernel = f.packet.kernel.state_;
          f.packet.input = tensor(f, kernel->tensorIndex(0));
        }
        const auto &source = f.packet.input.storage_;
        if (!source) {
          fail(task, f, std::int64_t(Error::Invalid));
          return;
        }
        std::uint32_t index = source->deviceIndex;
        const bool compute = f.packet.kind == Kind::Compute || f.packet.kind == Kind::Elementwise;
        const bool upload = f.packet.kind == Kind::Upload;
        const bool reduce = f.packet.kind == Kind::Reduce;
        const bool matrix = f.packet.kind == Kind::Matmul;
        std::array<std::int64_t, 2> matrixShape{};
        if (matrix) {
          const auto &kernel = f.packet.kernel.state_;
          const auto &right = f.packet.right.storage_;
          const auto format = kernel && kernel->numericType
                                  ? *storageFormat(f.packet.numericScalar
                                                       ? *kernel->numericType
                                                       : kernel->tensorType(kernel->tensorIndex(0)))
                                  : Format::Float32;
          const auto width = describeNumeric(*storageNumericType(format))->bytes();
          if (!kernel || !kernel->matrix || !right || source->format != format ||
              right->format != format || source->rank != 2 || right->rank != 2 ||
              source->dimensions[1] != right->dimensions[0] || source->size / width > UINT32_MAX ||
              right->size / width > UINT32_MAX) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
          for (auto dimension :
               {source->dimensions[0], source->dimensions[1], right->dimensions[1]})
            if (std::uint64_t(dimension) > UINT32_MAX) {
              fail(task, f, std::int64_t(Error::Invalid));
              return;
            }
          matrixShape = {source->dimensions[0], right->dimensions[1]};
          if (std::uint64_t(matrixShape[0]) * std::uint64_t(matrixShape[1]) > UINT32_MAX) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
          index = kernel->index;
          if (kernel->registry != state || !source->device || !right->device ||
              source->state != state || right->state != state || source->deviceIndex != index ||
              right->deviceIndex != index) {
            fail(task, f, std::int64_t(Error::Residency));
            return;
          }
        }
        if (reduce) {
          const auto &kernel = f.packet.kernel.state_;
          if (!kernel || !kernel->reduction || kernel->matrix ||
              source->format !=
                  (kernel->numericType ? *storageFormat(*kernel->numericType) : Format::Float32) ||
              ((!kernel->numericType && source->rank < 1) || source->rank > 8) ||
              source->size / describeNumeric(*storageNumericType(source->format))->bytes() >
                  UINT32_MAX) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
          index = kernel->index;
          if (kernel->registry != state || !source->device || source->state != state ||
              source->deviceIndex != index) {
            fail(task, f, std::int64_t(Error::Residency));
            return;
          }
          if (!source->size && !kernel->numericType && !f.packet.epilogue &&
              !(f.packet.graph && f.graphPhase == 1)) {
            task.complete(&f.scalar);
            return;
          }
        }
        if (compute) {
          const auto &kernel = f.packet.kernel.state_;
          const auto &right = f.packet.right.storage_;
          const auto signature =
              kernel && kernel->signature ? *kernel->signature : ElementwiseSignature{2, 0};
          const auto format = kernel && kernel->numericType
                                  ? *storageFormat(f.packet.numericScalar
                                                       ? *kernel->numericType
                                                       : kernel->tensorType(kernel->tensorIndex(0)))
                                  : Format::Float32;
          const auto width = describeNumeric(*storageNumericType(format))->bytes();
          if (!kernel || kernel->reduction || kernel->matrix ||
              kernel->elementwise != (f.packet.kind == Kind::Elementwise) ||
              (!f.packet.arguments && (kernel->projected || kernel->indirect() ||
                                       signature.tensors != 2 || signature.scalars != 0)) ||
              (f.packet.arguments &&
               (f.packet.arguments->tensors.size() != signature.tensors ||
                f.packet.arguments->scalarBits.size() != kernel->scalarWords())) ||
              std::uint32_t(f.packet.operation) > 2 || source->format != format ||
              (source->rank > 8 || (!kernel->numericType && source->rank < 1) ||
               (!kernel->elementwise && source->rank != 1)) ||
              source->size / width > UINT32_MAX ||
              (signature.tensors == 2 && !f.packet.arguments &&
               (!right || right->format != format || right->rank != source->rank ||
                source->size != right->size ||
                !std::equal(source->dimensions.begin(), source->dimensions.begin() + source->rank,
                            right->dimensions.begin())))) {
            fail(task, f, std::int64_t(Error::Invalid));
            return;
          }
          index = kernel->index;
          if (kernel->registry != state || !source->device || source->state != state ||
              source->deviceIndex != index ||
              (signature.tensors == 2 && !f.packet.arguments &&
               (!right->device || right->state != state || right->deviceIndex != index))) {
            fail(task, f, std::int64_t(Error::Residency));
            return;
          }
          if (f.packet.arguments)
            for (std::size_t i = 0; i < f.packet.arguments->tensors.size(); ++i) {
              const auto &other = tensor(f, i).storage_;
              const auto reads =
                  kernel->indirect()
                      ? std::span<const std::uint32_t>(kernel->indirectReads)
                      : std::span<const std::uint32_t>(kernel->reads.data(), kernel->readCount);
              const bool read = std::binary_search(reads.begin(), reads.end(), i);
              const bool broadcast = kernel->scalarBuffer(i) ||
                                     (f.packet.graph && f.graphPhase == 0 &&
                                      i >= f.packet.graph->products[f.graphStage].tensorCount());
              if (!other ||
                  other->format !=
                      (kernel->numericType ? *storageFormat(kernel->tensorType(i)) : format) ||
                  (broadcast &&
                   (other->size != describeNumeric(*storageNumericType(other->format))->bytes())) ||
                  (other->rank > 8 || (!kernel->numericType && other->rank < 1)) ||
                  (read && !broadcast &&
                   (other->rank != source->rank ||
                    other->size / describeNumeric(*storageNumericType(other->format))->bytes() !=
                        source->size / width ||
                    !std::equal(source->dimensions.begin(),
                                source->dimensions.begin() + source->rank,
                                other->dimensions.begin())))) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              if (!other->device || other->state != state || other->deviceIndex != index) {
                fail(task, f, std::int64_t(Error::Residency));
                return;
              }
            }
        } else if (upload) {
          if (f.packet.target.state_ != state) {
            fail(task, f, std::int64_t(Error::Residency));
            return;
          }
          index = f.packet.target.index_;
          if (source->device && (source->state != state || source->deviceIndex != index)) {
            fail(task, f, std::int64_t(Error::Residency));
            return;
          }
        } else if (!reduce && source->device && source->state != state) {
          fail(task, f, std::int64_t(Error::Residency));
          return;
        }
        if (!compute && !reduce && !matrix && source->device == upload) {
          task.complete(&f.packet.input);
          return;
        }
        unsigned fusedVariant = 0;
        bool fusePost = false;
        Kernel graphPost;
        std::shared_ptr<const Arguments> graphPostArguments;
        if (matrix && f.packet.graph && !f.graphPhase && f.packet.graph->reductions.empty() &&
            f.graphBatched < f.graphParameters.size()) {
          const auto &graph = *f.packet.graph;
          const auto next = f.graphStage + 1;
          if (next < graph.products.size() && !graph.maps.empty() && graph.maps[next]) {
            const auto &edge = graph.products[next];
            const auto value = graph.inputs + f.graphStage;
            const auto &post = graph.maps[next].state_;
            const auto work = graphStageWork(f, f.graphStage) + graphStageWork(f, next);
            const bool unary =
                edge.operands.empty()
                    ? edge.left == value && edge.right == value && graph.uses[value] == 2
                    : edge.operands.size() == 1 && edge.operands[0] == value &&
                          graph.uses[value] == 1;
            // Preserve the four-logical-stage and estimated-work cancellation
            // allowance, including the map that will no longer dispatch.
            if (unary && post->signature && post->signature->tensors == 1 &&
                work <= graphWorkLimit && f.graphWork <= graphWorkLimit - work) {
              graphPost = graph.maps[next];
              graphPostArguments = graph.mapArguments[next];
            }
          }
        }
        const auto numericPost =
            f.packet.numericProgram && f.numericPostWanted
                ? f.packet.numericProgram->code[f.packet.numericProgram->pc + 1].kernel
                : Kernel{};
        const auto &postKernel = numericPost ? numericPost
                                 : graphPost ? graphPost
                                             : f.packet.matrixPost;
        const auto &postArguments =
            numericPost ? f.packet.numericProgram->code[f.packet.numericProgram->pc + 1].arguments
            : graphPost ? graphPostArguments
                        : f.packet.postArguments;
        if (matrix && postKernel.state_ && postArguments && f.packet.kernel.state_->tiledMatrix &&
            (!f.packet.kernel.state_->numericType || numericPost) &&
            (!f.packet.graph || graphPost) && (!f.packet.numericProgram || numericPost) &&
            !f.packet.epilogue && !f.matrixStage && matrixShape[0] && matrixShape[1]) {
          const auto &base = f.packet.kernel.state_;
          const auto &post = postKernel.state_;
          auto layout = MatmulLayout::Scalar;
          if (base->matrixTiles[0] && base->matrixTiles[1])
            layout = selectMatmulLayout(std::uint32_t(matrixShape[0]),
                                        std::uint32_t(source->dimensions[1]),
                                        std::uint32_t(matrixShape[1]));
          const bool registers =
              base->registerMatrix &&
              useRegisterMatmul(std::uint32_t(matrixShape[0]), std::uint32_t(source->dimensions[1]),
                                std::uint32_t(matrixShape[1]));
          const bool grouped =
              registers && base->groupedMatrix && post->fusedMatrices[4] &&
              useGroupedMatmul(std::uint32_t(matrixShape[0]), std::uint32_t(source->dimensions[1]),
                               std::uint32_t(matrixShape[1]));
          const bool skinny =
              base->skinnyMatrix && post->fusedMatrices[5] &&
              useSkinnyMatmul(std::uint32_t(matrixShape[0]), std::uint32_t(source->dimensions[1]),
                              std::uint32_t(matrixShape[1]));
          fusedVariant = skinny      ? 5
                         : grouped   ? 4
                         : registers ? 3
                         : base->block64Matrix && source->dimensions[1] >= 512 &&
                                 matrixShape[0] >= 16 && matrixShape[1] >= 8 &&
                                 source->dimensions[1] % 64 == 0
                             ? 2
                         : base->block32Matrix && source->dimensions[1] >= 256 &&
                                 source->dimensions[1] % 32 == 0
                             ? 1
                             : 0;
          fusePost = (!post->numericType || numericPost) && !post->indirect() &&
                     post->fusedMatrices[fusedVariant] &&
                     (layout != MatmulLayout::Quad || registers);
          if (fusePost && graphPost) {
            const auto used = state->bytes.load();
            const auto needed = std::uint64_t(matrixShape[0]) * matrixShape[1] * 4 + 12 +
                                4ull * post->parameterWords();
            if (used > state->options.maxBytes || needed > state->options.maxBytes - used)
              fusePost = false;
          }
          if (fusePost) {
            const auto &arguments = postArguments;
            const auto signature = post->signature.value_or(ElementwiseSignature{2, 0});
            if (arguments->tensors.size() != signature.tensors ||
                arguments->scalarBits.size() != post->scalarWords()) {
              fail(task, f, std::int64_t(Error::Invalid));
              return;
            }
            // Slot zero is the not-yet-materialized matrix, not its placeholder.
            // Validate every other logical capture exactly as the staged map.
            for (std::size_t i = 1; i < arguments->tensors.size(); ++i) {
              const auto &other = arguments->tensors[i].storage_;
              const bool read =
                  std::binary_search(post->reads.begin(), post->reads.begin() + post->readCount, i);
              if (!other || other->format != Format::Float32 || other->rank < 1 ||
                  other->rank > 8 ||
                  (read && (other->rank != 2 ||
                            other->size !=
                                std::uint64_t(matrixShape[0]) * std::uint64_t(matrixShape[1]) * 4 ||
                            other->dimensions[0] != matrixShape[0] ||
                            other->dimensions[1] != matrixShape[1]))) {
                fail(task, f, std::int64_t(Error::Invalid));
                return;
              }
              if (!other->device || other->state != state || other->deviceIndex != index) {
                fail(task, f, std::int64_t(Error::Residency));
                return;
              }
            }
          }
        }
        if (!f.active) {
          auto count = state->inFlight.load();
          do {
            if (count >= state->options.maxInFlight) {
              fail(task, f, std::int64_t(Error::Budget));
              return;
            }
          } while (!state->inFlight.compare_exchange_weak(count, count + 1));
          f.active = true;
        }
        const auto encodingStart = state->options.profile ? std::chrono::steady_clock::now()
                                                          : std::chrono::steady_clock::time_point{};
        id<MTLCommandBuffer> command = nil;
        if (compute && f.packet.kernel.state_->transform) {
          command = tensorTransformCommand(f);
          if (!command) {
            fail(task, f, std::int64_t(f.error));
            return;
          }
        } else if (reduce) {
          f.error = Error::Allocation;
          command = f.packet.kernel.state_->numericType ? numericReductionCommand(f)
                                                        : reductionCommand(f);
          if (!command) {
            fail(task, f, std::int64_t(f.error));
            return;
          }
          f.error = Error::Device;
        } else {
          auto reuse = reusableGraphMapResult(f);
          if (!reuse)
            reuse = reusableNumericMapResult(f);
          auto result =
              reuse ? std::expected<std::shared_ptr<Storage>, Error>(
                          std::const_pointer_cast<Storage>(reuse.storage_))
                    : allocateStorage(state,
                                      matrix ? std::span<const std::int64_t>(matrixShape)
                                             : f.packet.input.shape(),
                                      (compute || matrix) && f.packet.kernel.state_->numericType
                                          ? *storageFormat(*f.packet.kernel.state_->numericType)
                                          : source->format,
                                      upload || compute || matrix, index);
          if (!result) {
            fail(task, f, std::int64_t(result.error()));
            return;
          }
          f.result = Buffer(*result);
          const bool numeric = compute && f.packet.kernel.state_->numericType.has_value();
          if (!f.result.bytes() && numeric) {
            // A zero-element map can still contain checked uniform scalar work.
            // Bind an owned dummy word and execute its uniform validation once.
            const auto &kernel = *f.packet.kernel.state_;
            auto width = std::max(describeNumeric(*kernel.numericType)->bytes(), 4u);
            for (std::uint32_t i = 0; i < kernel.readCount; ++i)
              width = std::max(width,
                               describeNumeric(kernel.tensorType(kernel.tensorIndex(i)))->bytes());
            auto dummy = allocateMetal(state, index, width, false);
            if (!dummy) {
              fail(task, f, std::int64_t(dummy.error()));
              return;
            }
            (*result)->metal = *dummy;
          }
          if (!f.result.bytes() && !numeric) {
            // Empty composed results still validate both stages and all
            // arguments.
            if (f.packet.numericProgram || f.packet.graph ||
                (f.packet.reduction &&
                 (f.packet.epilogue || f.packet.reduction.state_->numericType)) ||
                f.packet.matrixPost || f.matrixStage == 1 || f.matrixStage == 2) {
              // Empty first stages submit no command; release their admission
              // before continuing iteratively, including arbitrarily long empty
              // graphs.
              --state->inFlight;
              f.active = false;
              f.succeeded = true;
              continue;
            } else
              task.complete(f.packet.reduction ? static_cast<const void *>(&f.scalar) : &f.result);
            return;
          }

          if (compute || matrix) {
            const auto &kernel = f.packet.kernel.state_;
            const auto tensors = kernel->readCount;
            const auto parameterBytes =
                matrix && !kernel->numericType ? 12 : 4ull * kernel->parameterWords();
            auto parametersBuffer = allocateMetal(state, index, parameterBytes, true);
            if (!parametersBuffer) {
              fail(task, f, std::int64_t(parametersBuffer.error()));
              return;
            }
            f.staging = *parametersBuffer;
            if (fusePost) {
              const auto &post = postKernel.state_;
              auto parameters = allocateMetal(state, index, 4ull * post->parameterWords(), true);
              if (!parameters) {
                fail(task, f, std::int64_t(parameters.error()));
                return;
              }
              f.fusedParameters = *parameters;
              auto *words = static_cast<std::uint32_t *>((*parameters)->buffer.contents);
              words[0] = std::uint32_t(std::uint64_t(matrixShape[0]) * matrixShape[1]);
              std::copy(postArguments->scalarBits.begin(), postArguments->scalarBits.end(),
                        words + 1);
              if (numericPost) {
                const auto &p = *f.packet.numericProgram;
                const auto &post = p.code[p.pc + 1];
                for (std::size_t i = 0; i < post.scalars.size(); ++i)
                  words[i + 1] = std::uint32_t(numericScalar(p, post, i));
                words[post.kernel.state_->parameterWords() - 1] = UINT32_MAX;
              }
            }
            if ((f.packet.graph && f.graphPhase == 2) || f.packet.numericScalar) {
              auto readback =
                  allocateMetal(state, index,
                                f.packet.numericScalar
                                    ? std::max(describeNumeric(*kernel->numericType)->bytes(), 4u)
                                    : 4,
                                true);
              if (!readback) {
                fail(task, f, std::int64_t(readback.error()));
                return;
              }
              f.joinReadback = *readback;
            }
            id<MTLArgumentEncoder> arguments = nil;
            if (kernel->indirect()) {
              auto resources = allocateMetal(state, index, kernel->encodedBytes, true);
              if (!resources) {
                fail(task, f, std::int64_t(resources.error()));
                return;
              }
              f.resources = *resources;
              // Argument encoders are invocation-local: their mutable
              // destination cannot be shared by concurrent calls using the same
              // immutable Kernel.
              arguments = state->permit()
                              ? [kernel->argumentFunction newArgumentEncoderWithBufferIndex:0]
                              : nil;
              if (!arguments) {
                fail(task, f, std::int64_t(Error::Allocation));
                return;
              }
              [arguments setArgumentBuffer:f.resources->buffer offset:0];
            }
            command = f.pendingCommand;
            if (!command &&
                (!state->permit() || !(command = [state->queues[index] commandBuffer]))) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            if (kernel->numericType && describeNumeric(*kernel->numericType)->bytes() < 4) {
              id<MTLBlitCommandEncoder> clear =
                  state->permit() ? [command blitCommandEncoder] : nil;
              if (!clear) {
                fail(task, f, std::int64_t(Error::Allocation));
                return;
              }
              [clear fillBuffer:(*result)->metal->buffer
                          range:NSMakeRange(0, (*result)->metal->bytes)
                          value:0];
              [clear endEncoding];
            }
            id<MTLComputeCommandEncoder> encoder =
                state->permit() ? [command computeCommandEncoder] : nil;
            if (!encoder) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            auto layout = MatmulLayout::Scalar;
            if (matrix && kernel->matrixTiles[0] && kernel->matrixTiles[1])
              layout = selectMatmulLayout(std::uint32_t(matrixShape[0]),
                                          std::uint32_t(source->dimensions[1]),
                                          std::uint32_t(matrixShape[1]));
            auto pipeline = layout == MatmulLayout::Rows2  ? kernel->matrixTiles[0]
                            : layout == MatmulLayout::Quad ? kernel->matrixTiles[1]
                                                           : kernel->pipeline;
            const bool hardwareRegister = matrix && kernel->registerMatrix &&
                                          useRegisterMatmul(std::uint32_t(matrixShape[0]),
                                                            std::uint32_t(source->dimensions[1]),
                                                            std::uint32_t(matrixShape[1]));
            // An older post-map can fuse its earlier matrix entry. Its grid
            // must stay paired with that entry even when the base has skinny.
            const bool hardwareSkinny =
                matrix && kernel->skinnyMatrix && (!fusePost || fusedVariant == 5) &&
                useSkinnyMatmul(std::uint32_t(matrixShape[0]), std::uint32_t(source->dimensions[1]),
                                std::uint32_t(matrixShape[1]));
            const bool hardwareTile =
                matrix && kernel->tiledMatrix && (layout != MatmulLayout::Quad || hardwareRegister);
            const bool hardwareWide = hardwareTile && !hardwareRegister && kernel->block64Matrix &&
                                      source->dimensions[1] >= 512 && matrixShape[0] >= 16 &&
                                      matrixShape[1] >= 8 && source->dimensions[1] % 64 == 0;
            if (hardwareTile)
              pipeline = hardwareRegister ? kernel->registerMatrix
                         : hardwareWide   ? kernel->block64Matrix
                         : kernel->block32Matrix && source->dimensions[1] >= 256 &&
                                 source->dimensions[1] % 32 == 0
                             ? kernel->block32Matrix
                             : kernel->tiledMatrix;
            if (hardwareRegister && kernel->groupedMatrix &&
                useGroupedMatmul(std::uint32_t(matrixShape[0]),
                                 std::uint32_t(source->dimensions[1]),
                                 std::uint32_t(matrixShape[1])))
              pipeline = kernel->groupedMatrix;
            if (hardwareSkinny)
              pipeline = kernel->skinnyMatrix;
            if (fusePost)
              pipeline = postKernel.state_->fusedMatrices[fusedVariant];
            [encoder setComputePipelineState:pipeline];
            if (!kernel->indirect()) {
              [encoder setBuffer:(matrix || kernel->numericType) && !source->size
                                     ? (*result)->metal->buffer
                                     : source->metal->buffer
                          offset:0
                         atIndex:0];
              if (f.packet.arguments) {
                for (std::size_t i = 0; i < tensors; ++i) {
                  const auto &input = tensor(f, kernel->tensorIndex(i)).storage_;
                  [encoder setBuffer:input->metal ? input->metal->buffer : (*result)->metal->buffer
                              offset:0
                             atIndex:i];
                }
              } else if (tensors == 2)
                [encoder setBuffer:matrix && !f.packet.right.bytes()
                                       ? (*result)->metal->buffer
                                       : f.packet.right.storage_->metal->buffer
                            offset:0
                           atIndex:1];
              [encoder setBuffer:(*result)->metal->buffer offset:0 atIndex:tensors];
            }
            auto *parameters = static_cast<std::uint32_t *>(f.staging->buffer.contents);
            std::fill_n(parameters, parameterBytes / 4, 0u);
            const auto width = describeNumeric(*storageNumericType(source->format))->bytes();
            parameters[0] = std::uint32_t(source->size / width);
            if (kernel->numericType)
              parameters[kernel->parameterWords() - 1] = UINT32_MAX;
            if (matrix) {
              parameters[0] = std::uint32_t(matrixShape[0]);
              parameters[1] = std::uint32_t(source->dimensions[1]);
              parameters[2] = std::uint32_t(matrixShape[1]);
            } else if (f.packet.numericProgram && !f.packet.numericProgram->captures.empty()) {
              const auto &program = *f.packet.numericProgram;
              const auto &op = program.code[program.pc];
              std::size_t word = 1;
              for (std::size_t i = 0; i < op.scalars.size(); ++i) {
                const auto bits = program.captures[op.scalarOffset + i];
                parameters[word++] = std::uint32_t(bits);
                if (describeNumeric(kernel->scalarType(i))->bytes() == 8)
                  parameters[word++] = std::uint32_t(bits >> 32);
              }
            } else if (f.packet.arguments)
              std::copy(f.packet.arguments->scalarBits.begin(),
                        f.packet.arguments->scalarBits.end(), parameters + 1);
            else
              parameters[1] = std::uint32_t(f.packet.operation);
            if (kernel->indirect()) {

              for (std::size_t i = 0; i < tensors; ++i) {
                const auto &input = tensor(f, kernel->tensorIndex(i)).storage_;
                auto buffer = input->metal ? input->metal->buffer : (*result)->metal->buffer;
                [arguments setBuffer:buffer offset:0 atIndex:i];
                [encoder useResource:buffer usage:MTLResourceUsageRead];
              }
              [arguments setBuffer:(*result)->metal->buffer offset:0 atIndex:tensors];
              [arguments setBuffer:f.staging->buffer offset:0 atIndex:tensors + 1];
              [encoder
                  useResource:(*result)->metal->buffer
                        usage:kernel->numericType ? MTLResourceUsageRead | MTLResourceUsageWrite
                                                  : MTLResourceUsageWrite];
              [encoder
                  useResource:f.staging->buffer
                        usage:kernel->numericType ? MTLResourceUsageRead | MTLResourceUsageWrite
                                                  : MTLResourceUsageRead];
              [encoder setBuffer:f.resources->buffer offset:0 atIndex:0];
            } else
              [encoder setBuffer:f.staging->buffer offset:0 atIndex:tensors + 1];
            if (fusePost) {
              const auto &post = postKernel.state_;
              for (std::uint32_t i = 1; i < post->readCount; ++i) {
                const auto &input = postArguments->tensors[post->tensorIndex(i)].storage_;
                [encoder setBuffer:input->metal->buffer offset:0 atIndex:i + 3];
              }
              [encoder setBuffer:f.fusedParameters->buffer offset:0 atIndex:post->readCount + 3];
            }
            const auto threads = hardwareSkinny || hardwareRegister ? 256u
                                 : hardwareWide                     ? 128u
                                                                    : 64u;
            const auto lanes =
                hardwareSkinny        ? std::uint64_t(parameters[0]) *
                                            ((std::uint64_t(parameters[2]) + 31) / 32) * 256
                : hardwareRegister    ? ((std::uint64_t(parameters[0]) + 15) / 16) *
                                            ((std::uint64_t(parameters[2]) + 15) / 16) * 256
                : hardwareWide        ? ((std::uint64_t(parameters[0]) + 15) / 16) *
                                            ((std::uint64_t(parameters[2]) + 7) / 8) * 128
                : hardwareTile        ? ((std::uint64_t(parameters[0]) + 7) / 8) *
                                            ((std::uint64_t(parameters[2]) + 7) / 8) * 64
                : matrix              ? matmulWorkItems(layout, parameters[0], parameters[2])
                : kernel->numericType ? std::max(UINT64_C(1), std::uint64_t(parameters[0]))
                                      : std::uint64_t(parameters[0]);
            if (state->options.profile)
              ++state->dispatches;
            [encoder dispatchThreadgroups:MTLSizeMake((lanes + threads - 1) / threads, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
            [encoder endEncoding];
            if (fusePost) {
              f.fusedPostKernel = postKernel;
              f.numericFused = bool(numericPost);
              if (graphPost) {
                f.graphWork += graphStageWork(f, f.graphStage);
                ++f.graphStage;
                ++f.graphBatched;
              } else
                f.packet.matrixPost = {};
            }
            if ((f.packet.graph && f.graphPhase == 2) || f.packet.numericScalar) {
              id<MTLBlitCommandEncoder> blit = state->permit() ? [command blitCommandEncoder] : nil;
              if (!blit) {
                fail(task, f, std::int64_t(Error::Allocation));
                return;
              }
              [blit copyFromBuffer:f.result.storage_->metal->buffer
                       sourceOffset:0
                           toBuffer:f.joinReadback->buffer
                  destinationOffset:0
                               size:f.packet.numericScalar
                                        ? describeNumeric(*kernel->numericType)->bytes()
                                        : 4];
              [blit endEncoding];
            }
            if (!state->permit()) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
          } else {
            auto staging = allocateMetal(state, index, source->size, true);
            if (!staging) {
              fail(task, f, std::int64_t(staging.error()));
              return;
            }
            f.staging = *staging;
            if (upload)
              std::memcpy(f.staging->buffer.contents, source->hostView().data(), source->size);
            if (!state->permit()) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            command = [state->queues[index] commandBuffer];
            if (!command) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            if (!state->permit()) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (!blit) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
            if (upload) {
              if ((*result)->metal->bytes != source->size)
                [blit fillBuffer:(*result)->metal->buffer
                           range:NSMakeRange(0, (*result)->metal->bytes)
                           value:0];
              [blit copyFromBuffer:f.staging->buffer
                       sourceOffset:0
                           toBuffer:(*result)->metal->buffer
                  destinationOffset:0
                               size:source->size];
            } else
              [blit copyFromBuffer:source->metal->buffer
                       sourceOffset:0
                           toBuffer:f.staging->buffer
                  destinationOffset:0
                               size:source->size];
            [blit endEncoding];
            if (!state->permit()) {
              fail(task, f, std::int64_t(Error::Allocation));
              return;
            }
          }
        }
        if (f.packet.numericProgram && f.numericEligible) {
          auto &program = *f.packet.numericProgram;
          const auto &op = program.code[program.pc];
          f.numericWork += *numericBatchWork(f);
          const auto slot = f.numericBatched++;
          f.numericFlagWords[slot] = op.kernel.state_->parameterWords() - 1;
          f.numericParameters[slot] = std::move(f.staging);
          f.numericResources[slot] = std::move(f.resources);
          f.numericScratch[slot] = std::move(f.transformScratch);
          if (f.numericFused) {
            const auto postSlot = f.numericBatched++;
            f.numericFlagWords[postSlot] =
                program.code[program.pc + 1].kernel.state_->parameterWords() - 1;
            f.numericParameters[postSlot] = std::move(f.fusedParameters);
          }
          program.deferRetirement = true;
          finishNumeric(f);
          f.packet.input = {};
          f.packet.right = {};
          f.packet.kernel = {};
          f.packet.arguments.reset();
          f.pendingCommand = command;
          f.started = false;
          if (state->options.profile)
            state->encodingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - encodingStart)
                                              .count();
          if (task.cancelled())
            return;
          continue;
        }
        if (batchGraphStage(f)) {
          f.graphWork += graphStageWork(f, f.graphStage);
          f.graphParameters[f.graphBatched] = std::move(f.staging);
          f.graphPostParameters[f.graphBatched] = std::move(f.fusedParameters);
          f.graphResources[f.graphBatched++] = std::move(f.resources);
          f.packet.graph->values[f.packet.graph->inputs + f.graphStage] = std::move(f.result);
          ++f.graphStage;
          f.packet.input = {};
          f.packet.right = {};
          f.packet.arguments.reset();
          f.pendingCommand = command;
          f.started = false;
          if (state->options.profile)
            state->encodingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - encodingStart)
                                              .count();
          if (task.cancelled())
            return;
          continue;
        }
        // Hardware float32 post-maps have no checked device fault or observable
        // host boundary. Batch only with ample workspace headroom; tight budgets
        // retain the acknowledgement path and its earlier resource reclamation.
        const auto usedBytes = state->bytes.load();
        const auto postBytes = f.packet.matrixPost.state_
                                   ? 4ull * f.packet.matrixPost.state_->parameterWords() +
                                         f.packet.matrixPost.state_->encodedBytes
                                   : 0;
        const bool batchPost =
            matrix && f.packet.matrixPost.state_ && f.packet.kernel.state_->tiledMatrix &&
            !f.packet.matrixPost.state_->numericType && !f.packet.graph &&
            !f.packet.numericProgram && !f.packet.epilogue &&
            (!f.packet.reduction || f.packet.reduction.state_->blockReduction) &&
            f.result.bytes() && usedBytes <= state->options.maxBytes &&
            2 * f.result.bytes() + postBytes + 4096 <= state->options.maxBytes - usedBytes;
        if (batchPost) {
          f.batchedLeft = std::move(f.packet.input);
          f.batchedRight = std::move(f.packet.right);
          f.batchedKernel = std::move(f.packet.kernel);
          f.batchedParameters = std::move(f.staging);
          f.pendingCommand = command;
          f.packet.replacement = std::move(f.result);
          f.packet.kernel = std::move(f.packet.matrixPost);
          f.packet.arguments = std::move(f.packet.postArguments);
          f.packet.kind = Kind::Elementwise;
          f.matrixStage = 0;
          f.started = false;
          if (state->options.profile)
            state->encodingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - encodingStart)
                                              .count();
          if (task.cancelled())
            return;
          continue;
        }
        // A pure hardware map/matrix followed by a sum has no observable host
        // boundary. Encode the reduction into the same command, retaining every
        // original resource and its budget reservation through acknowledgement.
        const bool finalGraphReduction = batchGraphReduction(f);
        if ((compute || matrix) && f.packet.reduction.state_ &&
            f.packet.reduction.state_->blockReduction && !f.packet.kernel.state_->numericType &&
            (!f.packet.graph || finalGraphReduction) && !f.packet.numericProgram &&
            !f.packet.matrixPost && !f.packet.epilogue && !f.matrixStage) {
          if (finalGraphReduction)
            f.combinedGraph = std::move(f.packet.graph);
          f.combinedSource = std::move(f.packet.input);
          f.combinedKernel = std::move(f.packet.kernel);
          f.combinedParameters = std::move(f.staging);
          f.packet.input = std::move(f.result);
          f.packet.kernel = std::move(f.packet.reduction);
          f.packet.kind = Kind::Reduce;
          f.error = Error::Allocation;
          command = reductionCommand(f, command);
          if (!command) {
            fail(task, f, std::int64_t(f.error));
            return;
          }
        }
        if (state->options.profile)
          state->encodingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - encodingStart)
                                            .count();
        submitCommand(task, f, command);
        return;
      }
    }
  }
};
const ValueLayout Bridge::layout{0x4d4554414c01, sizeof(Buffer), alignof(Buffer), Bridge::copy,
                                 Bridge::destroyValue};
const TaskEntry Bridge::entry{1,
                              0x4d4554414c02,
                              sizeof(PacketData),
                              alignof(PacketData),
                              sizeof(Frame),
                              alignof(Frame),
                              Bridge::construct,
                              Bridge::destroy,
                              Bridge::resume,
                              &Bridge::layout};
const TaskEntry Bridge::reductionEntry{1,
                                       0x4d4554414c03,
                                       sizeof(PacketData),
                                       alignof(PacketData),
                                       sizeof(Frame),
                                       alignof(Frame),
                                       Bridge::construct,
                                       Bridge::destroy,
                                       Bridge::resume,
                                       numericScalarLayout(NumericType::Float32)};
Device::Device(std::shared_ptr<State> state, std::uint32_t index) noexcept
    : state_(std::move(state)), index_(index) {}
std::uint64_t Device::identity() const noexcept {
  return state_ ? state_->devices[index_].registryID : 0;
}
std::string_view Device::name() const noexcept {
  return state_ ? std::string_view(state_->names[index_]) : std::string_view{};
}
Buffer::Buffer(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}
std::uint64_t Buffer::bytes() const noexcept { return storage_ ? storage_->size : 0; }
std::span<const std::int64_t> Buffer::shape() const noexcept {
  return storage_ ? std::span<const std::int64_t>(storage_->dimensions.data(), storage_->rank)
                  : std::span<const std::int64_t>{};
}
bool Buffer::onDevice() const noexcept { return storage_ && storage_->device; }
Format Buffer::format() const noexcept { return storage_ ? storage_->format : Format::Float32; }
std::expected<std::span<const std::byte>, Error> Buffer::hostBytes() const noexcept {
  if (!storage_)
    return std::unexpected(Error::Invalid);
  if (storage_->device)
    return std::unexpected(Error::Residency);
  return storage_->hostView();
}
Runtime::Runtime(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
std::expected<std::unique_ptr<Runtime>, Error> Runtime::create(Options options) noexcept {
  @autoreleasepool {
    if (!options.maxBytes || !options.maxInFlight)
      return std::unexpected(Error::Invalid);
    try {
      auto state = std::make_shared<State>();
      state->options = options;
      NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
      state->devices = [devices
          sortedArrayUsingComparator:^NSComparisonResult(id<MTLDevice> a, id<MTLDevice> b) {
            return a.registryID < b.registryID   ? NSOrderedAscending
                   : a.registryID > b.registryID ? NSOrderedDescending
                                                 : NSOrderedSame;
          }];
      if (!state->devices.count || state->devices.count > UINT32_MAX ||
          options.defaultDevice >= state->devices.count)
        return std::unexpected(Error::Unavailable);
      NSMutableArray<id<MTLCommandQueue>> *queues = [NSMutableArray array];
      for (id<MTLDevice> device in state->devices) {
        state->names.emplace_back(device.name.UTF8String);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue)
          return std::unexpected(Error::Allocation);
        [queues addObject:queue];
      }
      state->queues = [queues copy];
      return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
}
std::uint32_t Runtime::deviceCount() const noexcept { return state_->devices.count; }
std::expected<Device, Error> Runtime::device() const noexcept {
  return device(state_->options.defaultDevice);
}
std::expected<Device, Error> Runtime::device(std::uint32_t index) const noexcept {
  if (index >= deviceCount())
    return std::unexpected(Error::Unavailable);
  return Device(state_, index);
}
std::expected<Buffer, Error> Runtime::hostTensor(std::span<const std::byte> data,
                                                 std::span<const std::int64_t> shape,
                                                 Format format) noexcept {
  auto bytes = tensorBytes(shape, format);
  if (!bytes)
    return std::unexpected(bytes.error());
  if (*bytes != data.size())
    return std::unexpected(Error::Invalid);
  auto storage = allocateStorage(state_, shape, format, false);
  if (!storage)
    return std::unexpected(storage.error());
  if (*bytes)
    std::memcpy((*storage)->host.data(), data.data(), *bytes);
  return Buffer(*storage);
}
std::expected<Buffer, Error> Runtime::fromCpu(const Invocation &context,
                                              CpuTensorView view) noexcept {
  const auto format = storageFormat(view.element);
  if (!format)
    return std::unexpected(Error::Invalid);
  auto retained = retainCpuTensor(context, view);
  if (!retained)
    return std::unexpected(Error::Invalid);
  if (!retained->bytes())
    return hostTensor({}, retained->shape(), *format);
  const auto allocationBytes = retained->retainedBytes();
  if (!state_->permit())
    return std::unexpected(Error::Allocation);
  try {
    auto storage = std::make_shared<Storage>(state_, false);
    if (!state_->reserve(allocationBytes))
      return std::unexpected(Error::Budget);
    storage->hostReserved = allocationBytes; // Charge the complete retained allocation.
    storage->format = *format;
    storage->cpuOffset = retained->offsetBytes();
    storage->size = retained->bytes();
    storage->rank = retained->shape().size();
    std::copy(retained->shape().begin(), retained->shape().end(), storage->dimensions.begin());
    storage->cpu = std::move(*retained).releaseAllocation();
    return Buffer(std::move(storage));
  } catch (...) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<void *, Error> Runtime::exportCpu(Invocation &context, const Buffer &buffer,
                                                NumericType element, std::span<std::int64_t> sizes,
                                                std::span<std::int64_t> strides) noexcept {
  if (!context.usable() || !buffer || storageNumericType(buffer.format()) != element)
    return std::unexpected(Error::Invalid);
  auto bytes = buffer.hostBytes();
  if (!bytes)
    return std::unexpected(bytes.error()); // Explicit download required.
  auto result = copyCpuTensorData(context, element, *bytes, buffer.shape(), sizes, strides);
  if (!result)
    return std::unexpected(result.error() == CpuTensorError::Allocation ? Error::Allocation
                                                                        : Error::Invalid);
  return *result;
}
TaskHandle Runtime::upload(TaskCore &core, GroupHandle group, Buffer buffer,
                           Device target) noexcept {
  Bridge::PacketData packet{
      state_, std::move(buffer), std::move(target), Bridge::Kind::Upload, {}, BinaryOperation::Add,
      {}};
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::download(TaskCore &core, GroupHandle group, Buffer buffer) noexcept {
  Bridge::PacketData packet{state_, std::move(buffer),    {}, Bridge::Kind::Download,
                            {},     BinaryOperation::Add, {}};
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::upload(Task &task, GroupHandle group, Buffer buffer, Device target) noexcept {
  Bridge::PacketData packet{
      state_, std::move(buffer), std::move(target), Bridge::Kind::Upload, {}, BinaryOperation::Add,
      {}};
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::download(Task &task, GroupHandle group, Buffer buffer) noexcept {
  Bridge::PacketData packet{state_, std::move(buffer),    {}, Bridge::Kind::Download,
                            {},     BinaryOperation::Add, {}};
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
std::expected<Kernel, Error> Runtime::loadBinary32Kernel(std::string_view path,
                                                         Device device) noexcept {
  return loadKernel(path, std::move(device), false);
}
std::expected<Kernel, Error> Runtime::loadElementwiseKernel(std::string_view path,
                                                            Device device) noexcept {
  return loadKernel(path, std::move(device), true);
}
std::expected<Kernel, Error>
Runtime::loadElementwiseKernel(std::string_view path, Device device,
                               ElementwiseSignature signature) noexcept {
  return loadKernel(path, std::move(device), true, signature);
}
std::expected<Kernel, Error>
Runtime::loadElementwiseKernel(std::string_view path, Device device, ElementwiseSignature signature,
                               std::span<const std::uint32_t> reads) noexcept {
  return loadKernel(path, std::move(device), true, signature, false, false, reads);
}
std::expected<Kernel, Error> Runtime::loadKernel(
    std::string_view path, Device device, bool elementwise,
    std::optional<ElementwiseSignature> signature, bool reduction, bool matrix,
    std::optional<std::span<const std::uint32_t>> reads, std::optional<NumericType> numericType,
    std::span<const NumericType> tensorTypes, std::span<const NumericType> scalarTypes,
    std::span<const std::uint8_t> scalarBuffers) noexcept {
  @autoreleasepool {
    if (numericType &&
        (!describeNumeric(*numericType) || !signature || !elementwise ||
         ((matrix || reduction) && signature->binding != ElementwiseBinding::Direct)))
      return std::unexpected(Error::Invalid);
    if ((!tensorTypes.empty() &&
         (!numericType || !signature || tensorTypes.size() != signature->tensors)) ||
        (!scalarTypes.empty() &&
         (!numericType || !signature || scalarTypes.size() != signature->scalars)))
      return std::unexpected(Error::Invalid);
    for (auto type : tensorTypes)
      if (!describeNumeric(type))
        return std::unexpected(Error::Invalid);
    for (auto type : scalarTypes)
      if (!describeNumeric(type))
        return std::unexpected(Error::Invalid);
    if ((matrix || reduction) && (!tensorTypes.empty() || !scalarTypes.empty()))
      return std::unexpected(Error::Invalid);
    if (!scalarBuffers.empty()) {
      if (!numericType || !signature || scalarBuffers.size() != signature->tensors)
        return std::unexpected(Error::Invalid);
      bool seen = false;
      for (auto role : scalarBuffers) {
        if (role > 1 || (seen && !role))
          return std::unexpected(Error::Invalid);
        seen |= role;
      }
    }
    const auto numericWidth = numericType ? describeNumeric(*numericType)->bytes() : 4;
    auto scalarWords =
        signature ? std::uint64_t(signature->scalars) * (numericWidth == 8 ? 2 : 1) : 1;
    if (!scalarTypes.empty()) {
      scalarWords = 0;
      for (auto type : scalarTypes)
        scalarWords += describeNumeric(type)->bytes() == 8 ? 2 : 1;
    }
    const auto parameterWords = 1 + scalarWords + unsigned(numericType.has_value());
    const auto physical = reads ? reads->size() : signature ? signature->tensors : 2;
    const bool indirect = signature && signature->binding == ElementwiseBinding::ArgumentBuffer;
    if ((signature &&
         std::uint32_t(signature->binding) > std::uint32_t(ElementwiseBinding::ArgumentBuffer)) ||
        (indirect && (reduction || matrix || !elementwise)))
      return std::unexpected(Error::Invalid);
    if ((signature && (!elementwise || (signature->tensors == 0 && !numericType) ||
                       signature->scalars == UINT32_MAX)) ||
        !(numericType && !indirect && physical == 0 ? true
          : indirect ? indirectElementwiseLayout(physical, signature->scalars)
                     : directElementwiseLayout(physical, signature ? signature->scalars : 0)))
      return std::unexpected(Error::Invalid);
    if (reads) {
      if (!signature || reduction || matrix)
        return std::unexpected(Error::Invalid);
      for (std::size_t i = 0; i < reads->size(); ++i)
        if ((*reads)[i] >= signature->tensors || (i && (*reads)[i - 1] >= (*reads)[i]))
          return std::unexpected(Error::Invalid);
    }
    if (device.state_ != state_)
      return std::unexpected(Error::Residency);
    if (indirect &&
        state_->devices[device.index_].argumentBuffersSupport != MTLArgumentBuffersTier2)
      return std::unexpected(Error::Unavailable);
    if (signature && 4ull * parameterWords > state_->devices[device.index_].maxBufferLength)
      return std::unexpected(Error::Unavailable);
    if (path.empty() || path.find('\0') != std::string_view::npos)
      return std::unexpected(Error::Invalid);
    try {
      if (!state_->permit())
        return std::unexpected(Error::Allocation);
      NSString *name = [[NSString alloc] initWithBytes:path.data()
                                                length:path.size()
                                              encoding:NSUTF8StringEncoding];
      if (!name)
        return std::unexpected(Error::Invalid);
      NSError *error = nil;
      id<MTLLibrary> library =
          [state_->devices[device.index_] newLibraryWithURL:[NSURL fileURLWithPath:name]
                                                      error:&error];
      if (!library)
        return std::unexpected(Error::Invalid);
      id<MTLFunction> argumentFunction = nil;
      std::uint64_t encodedBytes = 0;
      auto prepare = [&](NSString *entry, bool fused = false,
                         bool hardwareFloat32 =
                             false) -> std::expected<id<MTLComputePipelineState>, Error> {
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (!function)
          return std::unexpected(Error::Invalid);
        MTLComputePipelineReflection *reflection = nil;
        id<MTLComputePipelineState> pipeline = [state_->devices[device.index_]
            newComputePipelineStateWithFunction:function
                                        options:MTLPipelineOptionBufferTypeInfo |
                                                MTLPipelineOptionBindingInfo
                                     reflection:&reflection
                                          error:&error];
        if (!pipeline || pipeline.maxTotalThreadsPerThreadgroup < 64)
          return std::unexpected(Error::Unavailable);
        // Validate the private dynamic-array ABI, not the numerical contents of
        // arbitrary shaders.
        const auto tensors = physical;
        const auto parameterIndex = tensors + (fused ? 3 : 1),
                   bindingCount = tensors + (fused ? 4 : 2);
        const auto words = matrix ? (numericType && !hardwareFloat32 ? 4 : 3) : parameterWords;

        if (indirect) {
          if (reflection.bindings.count != 1)
            return std::unexpected(Error::Invalid);
          id<MTLBufferBinding> root = (id<MTLBufferBinding>)reflection.bindings[0];
          if (root.type != MTLBindingTypeBuffer || root.index != 0 ||
              !root.bufferPointerType.elementIsArgumentBuffer ||
              root.bufferDataType != MTLDataTypeStruct ||
              root.bufferStructType.members.count != bindingCount)
            return std::unexpected(Error::Invalid);
          if (!state_->permit())
            return std::unexpected(Error::Allocation);
          auto encoder = [function newArgumentEncoderWithBufferIndex:0];
          if (!encoder || encoder.encodedLength < sizeof(std::uint64_t) ||
              encoder.encodedLength > state_->devices[device.index_].maxBufferLength)
            return std::unexpected(Error::Unavailable);
          if (encoder.encodedLength != root.bufferDataSize ||
              encoder.alignment != root.bufferAlignment ||
              encoder.alignment < alignof(std::uint64_t) ||
              (encoder.alignment & (encoder.alignment - 1)))
            return std::unexpected(Error::Invalid);
          std::uint64_t last = 0;
          for (std::size_t i = 0; i < bindingCount; ++i) {
            MTLStructMember *member = root.bufferStructType.members[i];
            auto pointer = member.pointerType;
            auto fields = pointer.elementStructType.members;
            const auto count = i == parameterIndex ? words : 1;
            const auto type = i < tensors && !tensorTypes.empty()
                                  ? tensorTypes[reads ? (*reads)[i] : i]
                                  : numericType.value_or(NumericType::Float32);
            const auto width =
                i == parameterIndex ? 4 : std::max(describeNumeric(type)->bytes(), 4u);
            const auto wire = !numericType ? MTLDataTypeFloat
                              : width == 8 ? MTLDataTypeULong
                                           : MTLDataTypeUInt;
            if (member.dataType != MTLDataTypePointer || member.argumentIndex != i ||
                member.offset < last || member.offset % alignof(std::uint64_t) ||
                member.offset > encoder.encodedLength - sizeof(std::uint64_t) || !pointer ||
                pointer.elementIsArgumentBuffer || pointer.elementType != MTLDataTypeStruct ||
                pointer.alignment != width || pointer.dataSize != std::uint64_t(width) * count ||
                fields.count != 1)
              return std::unexpected(Error::Invalid);
            last = member.offset + sizeof(std::uint64_t);
            MTLStructMember *field = fields[0];
            auto array = field.arrayType;
            if (field.offset != 0 || field.dataType != MTLDataTypeArray || !array ||
                array.stride != width || array.arrayLength != count ||
                array.elementType != (i == parameterIndex ? MTLDataTypeUInt : wire))
              return std::unexpected(Error::Invalid);
          }
          argumentFunction = function;
          encodedBytes = encoder.encodedLength;
          return pipeline;
        }
        if (reflection.bindings.count != bindingCount)
          return std::unexpected(Error::Invalid);
        bool seen[directBufferEntries] = {};
        for (id<MTLBinding> argument in reflection.bindings) {
          const auto type = argument.index < tensors && !tensorTypes.empty()
                                ? tensorTypes[reads ? (*reads)[argument.index] : argument.index]
                                : numericType.value_or(NumericType::Float32);
          const auto wireWidth = std::max(describeNumeric(type)->bytes(), 4u);
          const auto wireType = !numericType || hardwareFloat32 || (fused && argument.index < 3)
                                    ? MTLDataTypeFloat
                                : wireWidth == 8 ? MTLDataTypeULong
                                                 : MTLDataTypeUInt;
          const bool shape = fused && argument.index == 3;
          const bool integer = shape || argument.index == parameterIndex;
          const auto count = shape ? 3u : argument.index == parameterIndex ? words : 1u;
          if (argument.type != MTLBindingTypeBuffer || argument.index >= bindingCount ||
              seen[argument.index] ||
              ((id<MTLBufferBinding>)argument).bufferDataSize !=
                  (integer ? 4ull * count : wireWidth))
            return std::unexpected(Error::Invalid);
          id<MTLBufferBinding> binding = (id<MTLBufferBinding>)argument;
          auto members = binding.bufferStructType.members;
          if (binding.bufferAlignment != (integer ? 4 : wireWidth) || members.count != 1)
            return std::unexpected(Error::Invalid);
          MTLStructMember *member = members[0];
          MTLArrayType *array = member.arrayType;
          if (member.offset != 0 || member.dataType != MTLDataTypeArray || !array ||
              array.stride != (integer ? 4 : wireWidth) || array.arrayLength != count ||
              array.elementType != (integer ? MTLDataTypeUInt : wireType))
            return std::unexpected(Error::Invalid);
          seen[argument.index] = true;
        }
        return pipeline;
      };
      NSString *numericName = nil;
      if (numericType)
        numericName = [(matrix      ? @"kalandra_numeric_v1_matmul_"
                        : reduction ? @"kalandra_numeric_v1_reduce_"
                                    : @"kalandra_numeric_v1_")
            stringByAppendingString:[[NSString alloc]
                                        initWithBytes:describeNumeric(*numericType)->name.data()
                                               length:describeNumeric(*numericType)->name.size()
                                             encoding:NSUTF8StringEncoding]];
      if (numericType && !matrix && !reduction && (!tensorTypes.empty() || !scalarTypes.empty())) {
        std::vector<NumericType> ts =
            tensorTypes.empty() ? std::vector<NumericType>(signature->tensors, *numericType)
                                : std::vector<NumericType>(tensorTypes.begin(), tensorTypes.end());
        std::vector<NumericType> ss =
            scalarTypes.empty() ? std::vector<NumericType>(signature->scalars, *numericType)
                                : std::vector<NumericType>(scalarTypes.begin(), scalarTypes.end());
        auto spelling = numericKernelName(*numericType, ts, ss);
        if (std::any_of(scalarBuffers.begin(), scalarBuffers.end(),
                        [](auto x) { return x != 0; })) {
          spelling += "_d_";
          for (auto role : scalarBuffers)
            spelling += role ? 's' : 't';
        }
        numericName = [[NSString alloc] initWithBytes:spelling.data()
                                               length:spelling.size()
                                             encoding:NSUTF8StringEncoding];
      }
      auto prepared = prepare(numericType   ? numericName
                              : matrix      ? @"kalandra_matmul"
                              : reduction   ? @"kalandra_reduce_sum"
                              : indirect    ? @"kalandra_elementwise_v3"
                              : signature   ? @"kalandra_elementwise_v2"
                              : elementwise ? @"kalandra_elementwise"
                                            : @"strict_binary32");
      if (!prepared)
        return std::unexpected(prepared.error());
      auto pipeline = *prepared;
      std::array<id<MTLComputePipelineState>, 2> matrixTiles{};
      // Performance profile qualified on this device. Other Apple GPUs retain
      // the strict scalar entry until their crossover measurements exist.
      if (matrix && !numericType &&
          [state_->devices[device.index_].name isEqualToString:@"Apple M2 Max"]) {
        const std::array<NSString *, 2> names{@"kalandra_matmul_2x1", @"kalandra_matmul_2x2"};
        for (unsigned i = 0; i < names.size(); ++i) {
          if (!state_->permit())
            return std::unexpected(Error::Allocation);
          auto tile = prepare(names[i]);
          if (!tile)
            return std::unexpected(tile.error());
          matrixTiles[i] = *tile;
        }
      }
      if (!state_->permit())
        return std::unexpected(Error::Allocation);
      Kernel kernel;
      auto metadata = std::make_shared<KernelState>(state_, device.index_, pipeline, elementwise,
                                                    reduction, matrix, signature, matrixTiles);
      if (!numericType && reduction &&
          [library newFunctionWithName:@"kalandra_reduce_sum_hardware_block"]) {
        auto block = prepare(@"kalandra_reduce_sum_hardware_block");
        if (!block)
          return std::unexpected(block.error());
        metadata->blockReduction = *block;
      }
      const bool nativeFloat32Matrix =
          matrix && (!numericType || *numericType == NumericType::Float32);
      if (nativeFloat32Matrix && [library newFunctionWithName:@"kalandra_matmul_hardware_tiled"]) {
        auto tiled = prepare(@"kalandra_matmul_hardware_tiled", false, true);
        if (!tiled)
          return std::unexpected(tiled.error());
        metadata->tiledMatrix = *tiled;
        if ([library newFunctionWithName:@"kalandra_matmul_hardware_block32"]) {
          auto block32 = prepare(@"kalandra_matmul_hardware_block32", false, true);
          if (!block32)
            return std::unexpected(block32.error());
          metadata->block32Matrix = *block32;
        }
        if ([library newFunctionWithName:@"kalandra_matmul_hardware_block64"]) {
          auto block64 = prepare(@"kalandra_matmul_hardware_block64", false, true);
          if (!block64)
            return std::unexpected(block64.error());
          if ((*block64).maxTotalThreadsPerThreadgroup >= 128)
            metadata->block64Matrix = *block64;
        }
        if ([state_->devices[device.index_].name isEqualToString:@"Apple M2 Max"] &&
            [library newFunctionWithName:@"kalandra_matmul_hardware_register16"]) {
          if (!state_->permit())
            return std::unexpected(Error::Allocation);
          auto registers = prepare(@"kalandra_matmul_hardware_register16", false, true);
          if (!registers)
            return std::unexpected(registers.error());
          if ((*registers).maxTotalThreadsPerThreadgroup >= 256)
            metadata->registerMatrix = *registers;
        }
      }
      if (metadata->registerMatrix &&
          [library newFunctionWithName:@"kalandra_matmul_hardware_grouped16"]) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        auto grouped = prepare(@"kalandra_matmul_hardware_grouped16", false, true);
        if (!grouped)
          return std::unexpected(grouped.error());
        if ((*grouped).maxTotalThreadsPerThreadgroup >= 256)
          metadata->groupedMatrix = *grouped;
      }
      metadata->numericType = numericType;
      if (nativeFloat32Matrix &&
          [state_->devices[device.index_].name isEqualToString:@"Apple M2 Max"] &&
          [library newFunctionWithName:@"kalandra_matmul_hardware_skinny32"]) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        auto skinny = prepare(@"kalandra_matmul_hardware_skinny32", false, true);
        if (!skinny)
          return std::unexpected(skinny.error());
        if ((*skinny).maxTotalThreadsPerThreadgroup >= 256)
          metadata->skinnyMatrix = *skinny;
      }
      if ((!numericType || *numericType == NumericType::Float32) && elementwise && !matrix &&
          !reduction && !indirect && physical >= 1 && physical <= directBufferEntries - 4 &&
          (!reads || (*reads)[0] == 0) &&
          [library newFunctionWithName:@"kalandra_matmul_fused_tiled"]) {
        const std::array<NSString *, 6> names{
            @"kalandra_matmul_fused_tiled",     @"kalandra_matmul_fused_block32",
            @"kalandra_matmul_fused_block64",   @"kalandra_matmul_fused_register16",
            @"kalandra_matmul_fused_grouped16", @"kalandra_matmul_fused_skinny32"};
        for (unsigned i = 0; i < names.size(); ++i) {
          if (i >= 4 && (![library newFunctionWithName:names[i]] ||
                         ![state_->devices[device.index_].name isEqualToString:@"Apple M2 Max"]))
            continue;
          if (!state_->permit())
            return std::unexpected(Error::Allocation);
          auto pipeline = prepare(names[i], true);
          if (!pipeline)
            return std::unexpected(pipeline.error());
          const unsigned threads = i >= 3 ? 256 : i == 2 ? 128 : 64;
          if ((*pipeline).maxTotalThreadsPerThreadgroup >= threads &&
              (i < 3 || [state_->devices[device.index_].name isEqualToString:@"Apple M2 Max"]))
            metadata->fusedMatrices[i] = *pipeline;
        }
      }
      if (!scalarBuffers.empty()) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        metadata->scalarBuffers.assign(scalarBuffers.begin(), scalarBuffers.end());
      }
      if (!tensorTypes.empty()) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        metadata->tensorTypes.assign(tensorTypes.begin(), tensorTypes.end());
      }
      if (!scalarTypes.empty()) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        metadata->scalarTypes.assign(scalarTypes.begin(), scalarTypes.end());
      }
      metadata->readCount = physical;
      metadata->projected = reads.has_value();
      if (indirect) {
        if (!state_->permit())
          return std::unexpected(Error::Allocation);
        metadata->indirectReads.resize(physical);
        metadata->argumentFunction = argumentFunction;
        metadata->encodedBytes = encodedBytes;
        for (std::uint32_t i = 0; i < physical; ++i)
          metadata->indirectReads[i] = reads ? (*reads)[i] : i;
      } else
        for (std::uint32_t i = 0; i < physical; ++i)
          metadata->reads[i] = reads ? (*reads)[i] : i;
      kernel.state_ = std::move(metadata);
      return kernel;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
}
std::expected<Kernel, Error>
Runtime::loadTensorTransformKernel(std::string_view path, Device device, TensorTransformKind kind,
                                   NumericType type, bool deviceEpsilon) noexcept {
  @autoreleasepool {
    if (!describeNumeric(type) || unsigned(kind) > unsigned(TensorTransformKind::RmsNormRows) ||
        (kind != TensorTransformKind::Transpose && type != NumericType::Float32) ||
        (deviceEpsilon && kind != TensorTransformKind::RmsNormRows) || path.empty() ||
        path.find('\0') != std::string_view::npos)
      return std::unexpected(Error::Invalid);
    if (device.state_ != state_ || device.index_ >= state_->devices.count)
      return std::unexpected(Error::Residency);
    try {
      if (!state_->permit())
        return std::unexpected(Error::Allocation);
      NSString *name = [[NSString alloc] initWithBytes:path.data()
                                                length:path.size()
                                              encoding:NSUTF8StringEncoding];
      if (!name)
        return std::unexpected(Error::Invalid);
      NSError *error = nil;
      id<MTLLibrary> library =
          [state_->devices[device.index_] newLibraryWithURL:[NSURL fileURLWithPath:name]
                                                      error:&error];
      if (!library)
        return std::unexpected(Error::Invalid);
      if (!state_->permit())
        return std::unexpected(Error::Allocation);
      auto function = [library newFunctionWithName:@"kalandra_tensor_transform_v1"];
      if (!function)
        return std::unexpected(Error::Invalid);
      MTLComputePipelineReflection *reflection = nil;
      auto pipeline = [state_->devices[device.index_]
          newComputePipelineStateWithFunction:function
                                      options:MTLPipelineOptionBufferTypeInfo |
                                              MTLPipelineOptionBindingInfo
                                   reflection:&reflection
                                        error:&error];
      if (!pipeline || pipeline.maxTotalThreadsPerThreadgroup < 256)
        return std::unexpected(Error::Unavailable);
      std::array<bool, 5> seen{};
      for (id<MTLBinding> binding in reflection.bindings) {
        if (binding.type != MTLBindingTypeBuffer || binding.index >= seen.size() ||
            seen[binding.index])
          return std::unexpected(Error::Invalid);
        seen[binding.index] = true;
      }
      if (!seen[0] || !seen[2] || !seen[4] ||
          (kind != TensorTransformKind::Transpose && !seen[3]) || (deviceEpsilon && !seen[1]))
        return std::unexpected(Error::Invalid);
      if (!state_->permit())
        return std::unexpected(Error::Allocation);
      auto metadata = std::make_shared<KernelState>();
      metadata->registry = state_;
      metadata->index = device.index_;
      metadata->pipeline = pipeline;
      metadata->elementwise = true;
      metadata->matrix = metadata->reduction = false;
      metadata->numericType = type;
      metadata->transform = kind;
      const bool rms = kind == TensorTransformKind::RmsNormRows;
      metadata->signature =
          ElementwiseSignature{deviceEpsilon ? 2u : 1u, rms && !deviceEpsilon ? 1u : 0u};
      metadata->readCount = deviceEpsilon ? 2 : 1;
      metadata->reads[0] = 0;
      metadata->tensorTypes = {type};
      metadata->scalarBuffers = {0};
      if (deviceEpsilon) {
        metadata->reads[1] = 1;
        metadata->tensorTypes.push_back(type);
        metadata->scalarBuffers.push_back(1);
      } else if (rms)
        metadata->scalarTypes = {type};
      Kernel result;
      result.state_ = std::move(metadata);
      return result;
    } catch (...) {
      return std::unexpected(Error::Allocation);
    }
  }
}
std::expected<Kernel, Error>
Runtime::loadNumericKernel(std::string_view path, Device device, NumericType type,
                           ElementwiseSignature signature,
                           std::span<const std::uint32_t> reads) noexcept {
  return loadKernel(path, std::move(device), true, signature, false, false,
                    reads.empty() ? std::optional<std::span<const std::uint32_t>>{} : reads, type);
}
std::expected<Kernel, Error>
Runtime::loadNumericKernel(std::string_view path, Device device, NumericType result,
                           ElementwiseSignature signature, std::span<const NumericType> tensors,
                           std::span<const NumericType> scalars,
                           std::span<const std::uint32_t> reads) noexcept {
  return loadKernel(path, std::move(device), true, signature, false, false,
                    reads.empty() ? std::optional<std::span<const std::uint32_t>>{} : reads, result,
                    tensors, scalars);
}
std::expected<Kernel, Error> Runtime::loadNumericGraphKernel(
    std::string_view path, Device device, NumericType type, ElementwiseSignature signature,
    std::span<const NumericType> tensors, std::span<const NumericType> scalars,
    std::span<const std::uint32_t> reads, std::span<const std::uint8_t> roles) noexcept {
  return loadKernel(path, std::move(device), true, signature, false, false,
                    std::optional<std::span<const std::uint32_t>>{reads}, type, tensors, scalars,
                    roles);
}
std::expected<Kernel, Error> Runtime::loadNumericMatmulKernel(std::string_view path, Device device,
                                                              NumericType type) noexcept {
  return loadKernel(path, std::move(device), true, ElementwiseSignature{2, 0}, false, true, {},
                    type);
}
std::expected<Kernel, Error> Runtime::loadNumericReductionKernel(std::string_view path,
                                                                 Device device,
                                                                 NumericType type) noexcept {
  return loadKernel(path, std::move(device), true, ElementwiseSignature{1, 0}, true, false, {},
                    type);
}
std::expected<Kernel, Error> Runtime::loadReductionKernel(std::string_view path,
                                                          Device device) noexcept {
  return loadKernel(path, std::move(device), true, ElementwiseSignature{1, 0}, true);
}
std::expected<Kernel, Error> Runtime::loadMatmulKernel(std::string_view path,
                                                       Device device) noexcept {
  return loadKernel(path, std::move(device), true, {}, false, true);
}
TaskHandle Runtime::matmul(TaskCore &core, GroupHandle group, Kernel kernel, Buffer left,
                           Buffer right, Kernel post, std::span<const Buffer> postInputs,
                           std::span<const float> scalars, Kernel leftMap, Kernel rightMap,
                           std::span<const Buffer> mapInputs,
                           std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Matmul,
                            std::move(kernel),
                            BinaryOperation::Add,
                            std::move(right)};
  if (post) {
    auto arguments = Bridge::capture(state_, post, postInputs, scalars);
    if (arguments)
      packet.postArguments = *arguments;
    else
      packet.preparation = arguments.error();
    packet.matrixPost = std::move(post);
  } else if (!postInputs.empty() || !scalars.empty())
    packet.preparation = Error::Invalid;
  Bridge::matrixMaps(packet, std::move(leftMap), std::move(rightMap), mapInputs, mapScalars);
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmul(Task &task, GroupHandle group, Kernel kernel, Buffer left, Buffer right,
                           Kernel post, std::span<const Buffer> postInputs,
                           std::span<const float> scalars, Kernel leftMap, Kernel rightMap,
                           std::span<const Buffer> mapInputs,
                           std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Matmul,
                            std::move(kernel),
                            BinaryOperation::Add,
                            std::move(right)};
  if (post) {
    auto arguments = Bridge::capture(state_, post, postInputs, scalars);
    if (arguments)
      packet.postArguments = *arguments;
    else
      packet.preparation = arguments.error();
    packet.matrixPost = std::move(post);
  } else if (!postInputs.empty() || !scalars.empty())
    packet.preparation = Error::Invalid;
  Bridge::matrixMaps(packet, std::move(leftMap), std::move(rightMap), mapInputs, mapScalars);
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulGraph(TaskCore &task, GroupHandle group, Kernel kernel,
                                std::span<const Buffer> inputs,
                                std::span<const MatrixProduct> products,
                                std::span<const Kernel> maps,
                                std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(kernel), BinaryOperation::Add, {}};
  auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars);
  if (graph)
    packet.graph = *graph;
  else
    packet.preparation = graph.error();
  return task.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulGraph(Task &task, GroupHandle group, Kernel kernel,
                                std::span<const Buffer> inputs,
                                std::span<const MatrixProduct> products,
                                std::span<const Kernel> maps,
                                std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(kernel), BinaryOperation::Add, {}};
  auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars);
  if (graph)
    packet.graph = *graph;
  else
    packet.preparation = graph.error();
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulGraphReduceSum(TaskCore &task, GroupHandle group, Kernel kernel,
                                         std::span<const Buffer> inputs,
                                         std::span<const MatrixProduct> products, Kernel reduction,
                                         Kernel epilogue, std::span<const float> scalars,
                                         std::span<const Kernel> maps,
                                         std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(kernel), BinaryOperation::Add, {}};
  auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars);
  if (graph)
    packet.graph = *graph;
  else
    packet.preparation = graph.error();
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (epilogue) {
    auto args = Bridge::capture(state_, epilogue, std::array{Buffer{}}, scalars);
    if (args)
      packet.scalarArguments = *args;
    else
      packet.preparation = args.error();
    packet.epilogue = std::move(epilogue);
  } else if (!scalars.empty())
    packet.preparation = Error::Invalid;
  return task.submit(group, Bridge::reductionEntry,
                     {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulGraphReduceSum(Task &task, GroupHandle group, Kernel kernel,
                                         std::span<const Buffer> inputs,
                                         std::span<const MatrixProduct> products, Kernel reduction,
                                         Kernel epilogue, std::span<const float> scalars,
                                         std::span<const Kernel> maps,
                                         std::span<const float> mapScalars) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(kernel), BinaryOperation::Add, {}};
  auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars);
  if (graph)
    packet.graph = *graph;
  else
    packet.preparation = graph.error();
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (epilogue) {
    auto args = Bridge::capture(state_, epilogue, std::array{Buffer{}}, scalars);
    if (args)
      packet.scalarArguments = *args;
    else
      packet.preparation = args.error();
    packet.epilogue = std::move(epilogue);
  } else if (!scalars.empty())
    packet.preparation = Error::Invalid;
  return task.spawn(group, Bridge::reductionEntry,
                    {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::graphTensor(TaskCore &task, GroupHandle group, Kernel base,
                                std::span<const Buffer> inputs,
                                std::span<const MatrixProduct> products, Kernel reduction,
                                std::span<const std::uint32_t> roots, std::span<const Kernel> maps,
                                std::span<const float> scalars,
                                std::span<const MatrixScalar> broadcasts) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(base), BinaryOperation::Add, {}};
  packet.reduction = std::move(reduction);
  Bridge::captureTensorGraph(packet, inputs, products, roots, maps, scalars, broadcasts);
  return task.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::graphTensor(Task &task, GroupHandle group, Kernel base,
                                std::span<const Buffer> inputs,
                                std::span<const MatrixProduct> products, Kernel reduction,
                                std::span<const std::uint32_t> roots, std::span<const Kernel> maps,
                                std::span<const float> scalars,
                                std::span<const MatrixScalar> broadcasts) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(base), BinaryOperation::Add, {}};
  packet.reduction = std::move(reduction);
  Bridge::captureTensorGraph(packet, inputs, products, roots, maps, scalars, broadcasts);
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::graphReductions(TaskCore &task, GroupHandle group, Kernel base,
                                    std::span<const Buffer> inputs,
                                    std::span<const MatrixProduct> products, Kernel reduction,
                                    Kernel join, std::span<const std::uint32_t> roots,
                                    std::span<const float> scalars, std::span<const Kernel> maps,
                                    std::span<const float> mapScalars,
                                    std::span<const MatrixScalar> broadcasts) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(base), BinaryOperation::Add, {}};
  if (roots.size() < 2)
    packet.preparation = Error::Invalid;
  else {
    auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars,
                                      roots, broadcasts);
    if (graph)
      packet.graph = *graph;
    else
      packet.preparation = graph.error();
  }
  packet.reduction = std::move(reduction);
  Bridge::captureJoin(packet, std::move(join), scalars);
  return task.submit(group, Bridge::reductionEntry,
                     {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::graphReductions(Task &task, GroupHandle group, Kernel base,
                                    std::span<const Buffer> inputs,
                                    std::span<const MatrixProduct> products, Kernel reduction,
                                    Kernel join, std::span<const std::uint32_t> roots,
                                    std::span<const float> scalars, std::span<const Kernel> maps,
                                    std::span<const float> mapScalars,
                                    std::span<const MatrixScalar> broadcasts) noexcept {
  Bridge::PacketData packet{
      state_, {}, {}, Bridge::Kind::Matmul, std::move(base), BinaryOperation::Add, {}};
  if (roots.size() < 2)
    packet.preparation = Error::Invalid;
  else {
    auto graph = Bridge::captureGraph(state_, packet.kernel, inputs, products, maps, mapScalars,
                                      roots, broadcasts);
    if (graph)
      packet.graph = *graph;
    else
      packet.preparation = graph.error();
  }
  packet.reduction = std::move(reduction);
  Bridge::captureJoin(packet, std::move(join), scalars);
  return task.spawn(group, Bridge::reductionEntry,
                    {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulReduceSum(TaskCore &task, GroupHandle group, Kernel matrix,
                                    Kernel reduction, Buffer left, Buffer right, Kernel leftMap,
                                    Kernel rightMap, std::span<const Buffer> mapInputs,
                                    std::span<const float> mapScalars, Kernel adjustment,
                                    std::span<const Buffer> adjustmentInputs,
                                    std::span<const float> adjustmentScalars, Kernel epilogue,
                                    std::span<const float> epilogueScalars) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Matmul,
                            std::move(matrix),
                            BinaryOperation::Add,
                            std::move(right)};
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (adjustment) {
    auto args = Bridge::capture(state_, adjustment, adjustmentInputs, adjustmentScalars);
    if (args)
      packet.postArguments = *args;
    else
      packet.preparation = args.error();
    packet.matrixPost = std::move(adjustment);
  } else if (!adjustmentInputs.empty() || !adjustmentScalars.empty())
    packet.preparation = Error::Invalid;
  if (epilogue) {
    auto args = Bridge::capture(state_, epilogue, std::array{packet.input}, epilogueScalars);
    if (args)
      packet.scalarArguments = *args;
    else
      packet.preparation = args.error();
    packet.epilogue = std::move(epilogue);
  } else if (!epilogueScalars.empty())
    packet.preparation = Error::Invalid;
  Bridge::matrixMaps(packet, std::move(leftMap), std::move(rightMap), mapInputs, mapScalars);
  auto entry = &Bridge::reductionEntry;
  if (packet.kernel.state_ && packet.kernel.state_->numericType) {
    const auto &base = packet.kernel.state_, &end = packet.reduction.state_;
    if (!end || !end->reduction || base->numericType != end->numericType ||
        base->registry != end->registry || base->index != end->index || packet.epilogue ||
        packet.matrixPost || packet.matrixLeftMap || packet.matrixRightMap)
      packet.preparation = Error::Invalid;
    entry = &Bridge::numericEntry(*base->numericType);
  }
  return task.submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::matmulReduceSum(Task &task, GroupHandle group, Kernel matrix, Kernel reduction,
                                    Buffer left, Buffer right, Kernel leftMap, Kernel rightMap,
                                    std::span<const Buffer> mapInputs,
                                    std::span<const float> mapScalars, Kernel adjustment,
                                    std::span<const Buffer> adjustmentInputs,
                                    std::span<const float> adjustmentScalars, Kernel epilogue,
                                    std::span<const float> epilogueScalars) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Matmul,
                            std::move(matrix),
                            BinaryOperation::Add,
                            std::move(right)};
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (adjustment) {
    auto args = Bridge::capture(state_, adjustment, adjustmentInputs, adjustmentScalars);
    if (args)
      packet.postArguments = *args;
    else
      packet.preparation = args.error();
    packet.matrixPost = std::move(adjustment);
  } else if (!adjustmentInputs.empty() || !adjustmentScalars.empty())
    packet.preparation = Error::Invalid;
  if (epilogue) {
    auto args = Bridge::capture(state_, epilogue, std::array{packet.input}, epilogueScalars);
    if (args)
      packet.scalarArguments = *args;
    else
      packet.preparation = args.error();
    packet.epilogue = std::move(epilogue);
  } else if (!epilogueScalars.empty())
    packet.preparation = Error::Invalid;
  Bridge::matrixMaps(packet, std::move(leftMap), std::move(rightMap), mapInputs, mapScalars);
  auto entry = &Bridge::reductionEntry;
  if (packet.kernel.state_ && packet.kernel.state_->numericType) {
    const auto &base = packet.kernel.state_, &end = packet.reduction.state_;
    if (!end || !end->reduction || base->numericType != end->numericType ||
        base->registry != end->registry || base->index != end->index || packet.epilogue ||
        packet.matrixPost || packet.matrixLeftMap || packet.matrixRightMap)
      packet.preparation = Error::Invalid;
    entry = &Bridge::numericEntry(*base->numericType);
  }
  return task.spawn(group, *entry, {entry->packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::reduceSum(TaskCore &core, GroupHandle group, Kernel kernel, Buffer input,
                              Kernel epilogue, std::span<const float> scalars) noexcept {
  Bridge::PacketData packet{
      state_, std::move(input), {}, Bridge::Kind::Reduce, std::move(kernel), BinaryOperation::Add,
      {}};
  if (epilogue) {
    auto arguments = Bridge::capture(state_, epilogue, std::array{packet.input}, scalars);
    if (arguments)
      packet.scalarArguments = *arguments;
    else
      packet.preparation = arguments.error();
    packet.epilogue = std::move(epilogue);
  } else if (!scalars.empty())
    packet.preparation = Error::Invalid;
  return core.submit(group, Bridge::reductionEntry,
                     {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::reduceSum(Task &task, GroupHandle group, Kernel kernel, Buffer input,
                              Kernel epilogue, std::span<const float> scalars) noexcept {
  Bridge::PacketData packet{
      state_, std::move(input), {}, Bridge::Kind::Reduce, std::move(kernel), BinaryOperation::Add,
      {}};
  if (epilogue) {
    auto arguments = Bridge::capture(state_, epilogue, std::array{packet.input}, scalars);
    if (arguments)
      packet.scalarArguments = *arguments;
    else
      packet.preparation = arguments.error();
    packet.epilogue = std::move(epilogue);
  } else if (!scalars.empty())
    packet.preparation = Error::Invalid;
  return task.spawn(group, Bridge::reductionEntry,
                    {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
const ValueLayout &Runtime::scalarLayout() noexcept {
  return *numericScalarLayout(NumericType::Float32);
}
TaskHandle Runtime::elementwiseReduceSum(TaskCore &core, GroupHandle group, Kernel mapping,
                                         Kernel reduction, std::span<const Buffer> tensors,
                                         std::span<const float> scalars, Kernel epilogue) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(mapping),
                            BinaryOperation::Add,
                            {}};
  auto arguments = Bridge::capture(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (epilogue) {
    packet.epilogue = std::move(epilogue);
    packet.scalarArguments = packet.arguments;
  }
  return core.submit(group, Bridge::reductionEntry,
                     {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::elementwiseReduceSum(Task &task, GroupHandle group, Kernel mapping,
                                         Kernel reduction, std::span<const Buffer> tensors,
                                         std::span<const float> scalars, Kernel epilogue) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(mapping),
                            BinaryOperation::Add,
                            {}};
  auto arguments = Bridge::capture(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  if (!reduction)
    packet.preparation = Error::Invalid;
  packet.reduction = std::move(reduction);
  if (epilogue) {
    packet.epilogue = std::move(epilogue);
    packet.scalarArguments = packet.arguments;
  }
  return task.spawn(group, Bridge::reductionEntry,
                    {Bridge::reductionEntry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::compute(TaskCore &core, GroupHandle group, Kernel kernel,
                            BinaryOperation operation, Buffer left, Buffer right) noexcept {
  Bridge::PacketData packet{
      state_,    std::move(left), {}, Bridge::Kind::Compute, std::move(kernel),
      operation, std::move(right)};
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::compute(Task &task, GroupHandle group, Kernel kernel, BinaryOperation operation,
                            Buffer left, Buffer right) noexcept {
  Bridge::PacketData packet{
      state_,    std::move(left), {}, Bridge::Kind::Compute, std::move(kernel),
      operation, std::move(right)};
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::elementwise(TaskCore &core, GroupHandle group, Kernel kernel, Buffer left,
                                Buffer right) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            std::move(right)};
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::elementwise(Task &task, GroupHandle group, Kernel kernel, Buffer left,
                                Buffer right) noexcept {
  Bridge::PacketData packet{state_,
                            std::move(left),
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            std::move(right)};
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::elementwise(TaskCore &core, GroupHandle group, Kernel kernel,
                                std::span<const Buffer> tensors,
                                std::span<const float> scalars) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            tensors.size() < 2 ? Buffer{} : tensors[1]};
  auto arguments = Bridge::capture(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  return core.submit(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::elementwise(Task &task, GroupHandle group, Kernel kernel,
                                std::span<const Buffer> tensors,
                                std::span<const float> scalars) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            tensors.size() < 2 ? Buffer{} : tensors[1]};
  auto arguments = Bridge::capture(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  return task.spawn(group, Bridge::entry, {Bridge::entry.packetLayout, &packet, sizeof(packet)});
}
bool NumericExecutable::on(Device device) const noexcept {
  return plan_ && device.state_ == plan_->state && device.index_ == plan_->index;
}
std::expected<NumericExecutable, Error>
Runtime::prepareNumericProgram(std::uint32_t inputCount, std::span<const NumericInstruction> code,
                               std::uint32_t result, NumericType element, bool scalar) noexcept {
  if (std::uint64_t(inputCount) * sizeof(Buffer) > state_->options.maxArgumentBytes)
    return std::unexpected(Error::Budget);
  try {
    if (!state_->permit())
      return std::unexpected(Error::Allocation);
    std::vector<Buffer> placeholders(inputCount);
    auto plan = Bridge::captureProgram(state_, placeholders, code, result, element, scalar, true);
    if (!plan)
      return std::unexpected(plan.error());
    return NumericExecutable(*plan);
  } catch (...) {
    return std::unexpected(Error::Allocation);
  }
}
TaskHandle Runtime::executeNumericProgram(TaskCore &core, GroupHandle group,
                                          const NumericExecutable &executable,
                                          std::span<const Buffer> inputs,
                                          std::span<const std::uint64_t> captures) noexcept {
  Bridge::PacketData packet{state_, {}, {}, Bridge::Kind::Elementwise, {}, BinaryOperation::Add,
                            {}};
  auto program = Bridge::instantiateProgram(state_, executable.plan_, inputs, captures);
  if (program)
    packet.numericProgram = *program;
  else
    packet.preparation = program.error();
  const auto &entry = executable.plan_ && executable.plan_->scalar
                          ? Bridge::numericEntry(executable.plan_->element)
                          : Bridge::entry;
  return core.submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::executeNumericProgram(Task &task, GroupHandle group,
                                          const NumericExecutable &executable,
                                          std::span<const Buffer> inputs,
                                          std::span<const std::uint64_t> captures) noexcept {
  Bridge::PacketData packet{state_, {}, {}, Bridge::Kind::Elementwise, {}, BinaryOperation::Add,
                            {}};
  auto program = Bridge::instantiateProgram(state_, executable.plan_, inputs, captures);
  if (program)
    packet.numericProgram = *program;
  else
    packet.preparation = program.error();
  const auto &entry = executable.plan_ && executable.plan_->scalar
                          ? Bridge::numericEntry(executable.plan_->element)
                          : Bridge::entry;
  return task.spawn(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::numericProgram(TaskCore &core, GroupHandle group,
                                   std::span<const Buffer> inputs,
                                   std::span<const NumericInstruction> code, std::uint32_t result,
                                   NumericType element, bool scalar) noexcept {
  Bridge::PacketData packet{state_, {}, {}, Bridge::Kind::Elementwise, {}, BinaryOperation::Add,
                            {}};
  auto program = Bridge::captureProgram(state_, inputs, code, result, element, scalar);
  if (program)
    packet.numericProgram = *program;
  else
    packet.preparation = program.error();
  const auto &entry = scalar ? Bridge::numericEntry(element) : Bridge::entry;
  return core.submit(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::numericProgram(Task &task, GroupHandle group, std::span<const Buffer> inputs,
                                   std::span<const NumericInstruction> code, std::uint32_t result,
                                   NumericType element, bool scalar) noexcept {
  Bridge::PacketData packet{state_, {}, {}, Bridge::Kind::Elementwise, {}, BinaryOperation::Add,
                            {}};
  auto program = Bridge::captureProgram(state_, inputs, code, result, element, scalar);
  if (program)
    packet.numericProgram = *program;
  else
    packet.preparation = program.error();
  const auto &entry = scalar ? Bridge::numericEntry(element) : Bridge::entry;
  return task.spawn(group, entry, {entry.packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::numericElementwise(TaskCore &core, GroupHandle group, Kernel kernel,
                                       std::span<const Buffer> tensors,
                                       std::span<const std::uint64_t> scalars, Kernel reduction,
                                       bool scalarResult) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            {}};
  packet.numericScalar = scalarResult;
  if (scalarResult) {
    const auto &kernel = packet.kernel.state_;
    if (!kernel || !kernel->numericType || !tensors.empty() || reduction)
      packet.preparation = Error::Invalid;
    else {
      auto input =
          allocateStorage(state_, {}, *storageFormat(*kernel->numericType), true, kernel->index);
      if (input)
        packet.input = Buffer(*input);
      else
        packet.preparation = input.error();
    }
  }
  auto arguments = Bridge::captureNumeric(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  const auto *entry = &Bridge::entry;
  if (scalarResult)
    entry = &Bridge::numericEntry(packet.kernel.state_ && packet.kernel.state_->numericType
                                      ? *packet.kernel.state_->numericType
                                      : NumericType::Float32);
  if (reduction) {
    const auto &base = packet.kernel.state_, &end = reduction.state_;
    if (!base || !end || !end->numericType || !end->reduction ||
        base->numericType != end->numericType || base->registry != end->registry ||
        base->index != end->index)
      packet.preparation = Error::Invalid;
    entry = &Bridge::numericEntry(base && base->numericType ? *base->numericType
                                                            : NumericType::Float32);
    packet.reduction = std::move(reduction);
  }
  return core.submit(group, *entry, {entry->packetLayout, &packet, sizeof(packet)});
}
TaskHandle Runtime::numericElementwise(Task &task, GroupHandle group, Kernel kernel,
                                       std::span<const Buffer> tensors,
                                       std::span<const std::uint64_t> scalars, Kernel reduction,
                                       bool scalarResult) noexcept {
  Bridge::PacketData packet{state_,
                            tensors.empty() ? Buffer{} : tensors[0],
                            {},
                            Bridge::Kind::Elementwise,
                            std::move(kernel),
                            BinaryOperation::Add,
                            {}};
  packet.numericScalar = scalarResult;
  if (scalarResult) {
    const auto &kernel = packet.kernel.state_;
    if (!kernel || !kernel->numericType || !tensors.empty() || reduction)
      packet.preparation = Error::Invalid;
    else {
      auto input =
          allocateStorage(state_, {}, *storageFormat(*kernel->numericType), true, kernel->index);
      if (input)
        packet.input = Buffer(*input);
      else
        packet.preparation = input.error();
    }
  }
  auto arguments = Bridge::captureNumeric(state_, packet.kernel, tensors, scalars);
  if (arguments)
    packet.arguments = *arguments;
  else
    packet.preparation = arguments.error();
  const auto *entry = &Bridge::entry;
  if (scalarResult)
    entry = &Bridge::numericEntry(packet.kernel.state_ && packet.kernel.state_->numericType
                                      ? *packet.kernel.state_->numericType
                                      : NumericType::Float32);
  if (reduction) {
    const auto &base = packet.kernel.state_, &end = reduction.state_;
    if (!base || !end || !end->numericType || !end->reduction ||
        base->numericType != end->numericType || base->registry != end->registry ||
        base->index != end->index)
      packet.preparation = Error::Invalid;
    entry = &Bridge::numericEntry(base && base->numericType ? *base->numericType
                                                            : NumericType::Float32);
    packet.reduction = std::move(reduction);
  }
  return task.spawn(group, *entry, {entry->packetLayout, &packet, sizeof(packet)});
}
const ValueLayout &Runtime::bufferLayout() noexcept { return Bridge::layout; }
Stats Runtime::stats() const noexcept {
  return {state_->gpuNanoseconds, state_->encodingNanoseconds, state_->dispatches,
          state_->bytes,          state_->peakBytes,           state_->hostBackings,
          state_->deviceBackings, state_->metalBuffers,        state_->stagingBuffers,
          state_->inFlight,       state_->submissions,         state_->completions,
          state_->attempts,       state_->argumentBytes,       state_->peakArgumentBytes,
          state_->argumentPacks};
}
void Runtime::failAllocationAt(std::uint64_t attempt) noexcept {
  state_->attempts = 0;
  state_->failAt = attempt;
}
void Runtime::failNextCompletion() noexcept { state_->failCompletion = true; }
void Runtime::holdCompletions(bool hold) noexcept {
  {
    std::lock_guard lock(state_->gateMutex);
    state_->hold = hold;
  }
  if (!hold)
    state_->gate.notify_all();
}
} // namespace kalandra::runtime::metal
