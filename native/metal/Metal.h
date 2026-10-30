#pragma once
#include "../include/kalandra/TensorTransform.h"
#include "../runtime/Tasks.h"
#include "../runtime/Tensor.h"
#include "ElementwiseLayout.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace kalandra::runtime::metal {
struct State;
struct Storage;
struct Bridge;
struct KernelState;
struct NumericProgramArguments;
enum class Error : std::int64_t {
  Allocation = 2,
  Invalid = 3,
  // Native codes 4/5 identify checked arithmetic/bounds faults. Metal setup
  // and command failures must remain Native when mixed programs inspect them.
  Unavailable = 104,
  Device = 105,
  Budget = 6,
  Residency = 7
};
enum class Format { Float32, UInt32, UInt8, Int8, Int16, UInt16, Int32, Int64, UInt64, Float64 };
// Storage identity is independent of per-operation shader admission.
constexpr std::optional<NumericType> storageNumericType(Format format) {
  switch (format) {
  case Format::Float32:
    return NumericType::Float32;
  case Format::UInt32:
    return NumericType::UInt32;
  case Format::UInt8:
    return NumericType::UInt8;
  case Format::Int8:
    return NumericType::Int8;
  case Format::Int16:
    return NumericType::Int16;
  case Format::UInt16:
    return NumericType::UInt16;
  case Format::Int32:
    return NumericType::Int32;
  case Format::Int64:
    return NumericType::Int64;
  case Format::UInt64:
    return NumericType::UInt64;
  case Format::Float64:
    return NumericType::Float64;
  }
  return {};
}
constexpr std::optional<Format> storageFormat(NumericType element) {
  switch (element) {
  case NumericType::Float32:
    return Format::Float32;
  case NumericType::UInt32:
    return Format::UInt32;
  case NumericType::UInt8:
    return Format::UInt8;
  case NumericType::Int8:
    return Format::Int8;
  case NumericType::Int16:
    return Format::Int16;
  case NumericType::UInt16:
    return Format::UInt16;
  case NumericType::Int32:
    return Format::Int32;
  case NumericType::Int64:
    return Format::Int64;
  case NumericType::UInt64:
    return Format::UInt64;
  case NumericType::Float64:
    return Format::Float64;
  default:
    return {};
  }
}
struct Options {
  bool profile = false;
  // Pre-encode bounded pure chunks; completion callbacks submit the next chunk.
  bool preencodedProgression = true;
  bool retainedProgressionReferences = false;
  std::uint64_t maxBytes = 64 * 1024 * 1024;
  std::uint32_t maxInFlight = 64, defaultDevice = 0;
  std::uint64_t maxArgumentBytes = 1024 * 1024;
  // Owned CPU-populated device snapshots on unified-memory hardware. Disable
  // to qualify the private-buffer/blit fallback; no host alias is exposed.
  bool sharedUploads = true;
  bool workBoundedProgression = true;
  // Invocation-private argument backing; false retains separate buffers.
  bool parameterArena = true;
  // Singleton/empty transpose views; false retains the device-copy control.
  bool contiguousTranspose = true;
  // Encode later chunks while the first runs; false retains full pre-encoding.
  bool overlappedEncoding = true;
  // Only an opaque closed runner with no independent progress can use this.
  // Ordinary TaskCore callers always retain bounded asynchronous execution.
  bool closedSynchronous = true;
  // Shared-upload quota within the combined idle-device pool. Zero disables uploads.
  // Only fully owned, completed backing can be retained.
  std::uint64_t maxCachedUploadBytes = 16 * 1024 * 1024;
  // Combined idle upload/prepared-output pool, still included in maxBytes.
  std::uint64_t maxCachedDeviceBytes = 16 * 1024 * 1024;
  // Full Float32 outputs of validated pure prepared progression only.
  bool cachePreparedOutputs = true;
};
struct Stats {
  std::uint64_t gpuNanoseconds = 0, encodingNanoseconds = 0, dispatches = 0;
  std::uint64_t bytes = 0, peakBytes = 0, hostBackings = 0, deviceBackings = 0;
  std::uint64_t metalBuffers = 0, stagingBuffers = 0, inFlight = 0;
  std::uint64_t submissions = 0, completions = 0, attempts = 0;
  std::uint64_t argumentBytes = 0, peakArgumentBytes = 0, argumentPacks = 0;
  // bytes/metalBuffers remain live-owner counters. Retained bytes include idle
  // device backing and share the maxBytes limit. Concurrent snapshots may vary.
  std::uint64_t retainedBytes = 0, peakRetainedBytes = 0;
  std::uint64_t cachedUploadBytes = 0, cachedUploadBuffers = 0;
  std::uint64_t uploadCacheHits = 0, uploadCacheMisses = 0;
  std::uint64_t cachedPreparedOutputBytes = 0, cachedPreparedOutputBuffers = 0;
  std::uint64_t preparedOutputCacheHits = 0, preparedOutputCacheMisses = 0;
};
class Device final {
public:
  Device() = default;
  explicit operator bool() const noexcept { return bool(state_); }
  std::uint64_t identity() const noexcept;
  std::string_view name() const noexcept;
  bool hasUnifiedMemory() const noexcept;

private:
  std::shared_ptr<State> state_;
  std::uint32_t index_ = 0;
  Device(std::shared_ptr<State>, std::uint32_t) noexcept;
  friend class Runtime;
  friend struct Bridge;
  friend class NumericExecutable;
};
// Private native bridge values. No writable bytes, Metal objects, or forged
// descriptors are exposed. Copies retain immutable backing independently.
class Buffer final {
public:
  Buffer() = default;
  explicit operator bool() const noexcept { return bool(storage_); }
  std::uint64_t bytes() const noexcept;
  std::span<const std::int64_t> shape() const noexcept;
  bool onDevice() const noexcept;
  Format format() const noexcept;
  std::expected<std::span<const std::byte>, Error> hostBytes() const noexcept;

private:
  std::shared_ptr<const Storage> storage_;
  explicit Buffer(std::shared_ptr<const Storage>) noexcept;
  friend class Runtime;
  friend struct Bridge;
};
// Trusted, compiler-produced binary32 kernel artifact; not an arbitrary shader
// API.
enum class BinaryOperation : std::uint32_t { Add, Multiply, MultiplyAddLeft };
class Kernel final {
public:
  Kernel() = default;
  explicit operator bool() const noexcept { return bool(state_); }

private:
  std::shared_ptr<KernelState> state_;
  friend class Runtime;
  friend struct Bridge;
};
// Trusted compiler-produced entry layout; scalar words preserve float32 bits.
struct ElementwiseSignature {
  std::uint32_t tensors = 1, scalars = 0;
  ElementwiseBinding binding = ElementwiseBinding::Direct;
  bool operator==(const ElementwiseSignature &) const = default;
};
// Operand indices address original inputs followed by earlier product results.
struct MatrixProduct {
  std::uint32_t left, right;
  // Optional sorted unique map operands. Capture copies them before submission;
  // left/right must be their first/last entries. Matrix stages leave this
  // empty.
  std::span<const std::uint32_t> operands{};
  std::size_t tensorCount() const noexcept {
    return operands.empty() ? (left == right ? 1u : 2u) : operands.size();
  }
};
// Broadcast one resident reduction into a later tensor map. Sorted by stage,
// then reduction.
struct MatrixScalar {
  std::uint32_t stage, reduction;
};
// Private, compiler-owned numerical control program. Value IDs address input
// buffers and instruction outputs. All spans are copied before admission.
enum class NumericInstructionKind {
  Map,
  Reduce,
  Matmul,
  Branch,
  Jump,
  Alias,
  EqualShape,
  Extent,
  MatrixShape,
  ShapeFault
};
struct NumericInstruction {
  NumericInstructionKind kind;
  Kernel kernel{};
  std::span<const std::uint32_t> inputs{};
  std::span<const std::uint64_t> scalars{};
  // Map target bit 0 is a compiler proof that checked arithmetic cannot fault.
  // Branch/Jump target remains a forward instruction index. Other bits reserved.
  std::uint32_t output = UINT32_MAX, target = 0;
  // Trusted compiler scheduling proof: hardware map DAG cost per output cell;
  // matrix value 2 denotes multiply/add work per contraction cell, transform 8.
  // Zero means unknown: retain the four-stage compatibility path.
  std::uint32_t workPerElement = 0;
};
// Immutable validated code. Captured words are supplied separately for each
// invocation, in Map instruction order; empty captures reuse prepared values.
// Shape-check constants stay immutable. Registry and device identity are retained.
class NumericExecutable final {
public:
  NumericExecutable() = default;
  explicit operator bool() const noexcept { return bool(plan_); }
  bool on(Device) const noexcept;

private:
  std::shared_ptr<const NumericProgramArguments> plan_;
  explicit NumericExecutable(std::shared_ptr<const NumericProgramArguments> p) noexcept
      : plan_(std::move(p)) {}
  friend class Runtime;
};
class Runtime final {
public:
  ~Runtime();
  // Release idle cache storage; live buffers and in-flight work are unaffected.
  void trimUploadCache() noexcept;
  void trimDeviceCache() noexcept;
  // Thread-safe policy change. Disabling also releases idle prepared outputs.
  // Live/pending owners are unaffected and cannot refill a disabled output pool.
  void setPreparedOutputCaching(bool enabled) noexcept;
  static std::expected<std::unique_ptr<Runtime>, Error> create(Options = {}) noexcept;
  std::uint32_t deviceCount() const noexcept;
  std::expected<Device, Error> device() const noexcept;
  std::expected<Device, Error> device(std::uint32_t index) const noexcept;
  std::expected<Buffer, Error> hostTensor(std::span<const std::byte>, std::span<const std::int64_t>,
                                          Format) noexcept;
  std::expected<Buffer, Error> fromCpu(const Invocation &, CpuTensorView) noexcept;
  template <NumericType Element, std::size_t Rank>
  std::expected<CpuTensor<Element, Rank>, Error> toCpuTyped(Invocation &context,
                                                            const Buffer &buffer) noexcept {
    CpuTensor<Element, Rank> descriptor;
    std::span<std::int64_t> sizes, strides;
    if constexpr (Rank != 0) {
      sizes = descriptor.sizes;
      strides = descriptor.strides;
    }
    auto data = exportCpu(context, buffer, Element, sizes, strides);
    if (!data)
      return std::unexpected(data.error());
    descriptor.allocated = descriptor.aligned = static_cast<NumericValue<Element> *>(*data);
    return descriptor;
  }
  template <std::size_t Rank>
  std::expected<Float32Tensor<Rank>, Error> toCpu(Invocation &context,
                                                  const Buffer &buffer) noexcept {
    return toCpuTyped<NumericType::Float32, Rank>(context, buffer);
  }
  TaskHandle upload(TaskCore &, GroupHandle, Buffer, Device) noexcept;
  TaskHandle download(TaskCore &, GroupHandle, Buffer) noexcept;
  TaskHandle upload(Task &, GroupHandle, Buffer, Device) noexcept;
  TaskHandle download(Task &, GroupHandle, Buffer) noexcept;
  std::expected<Kernel, Error> loadBinary32Kernel(std::string_view metallib, Device) noexcept;
  std::expected<Kernel, Error> loadElementwiseKernel(std::string_view metallib, Device) noexcept;
  std::expected<Kernel, Error> loadElementwiseKernel(std::string_view metallib, Device,
                                                     ElementwiseSignature) noexcept;
  // Logical tensor indices read by the compact physical binding table. Copied
  // on load.
  std::expected<Kernel, Error> loadElementwiseKernel(std::string_view metallib, Device,
                                                     ElementwiseSignature,
                                                     std::span<const std::uint32_t> reads) noexcept;
  // Typed integer-bit shader ABI. Scalar bit packets must use the exact source
  // width. The parameter block includes a device-written first-fault index.
  std::expected<Kernel, Error>
  loadNumericKernel(std::string_view metallib, Device, NumericType, ElementwiseSignature,
                    std::span<const std::uint32_t> reads = {}) noexcept;
  std::expected<Kernel, Error>
  loadNumericKernel(std::string_view, Device, NumericType, ElementwiseSignature,
                    std::span<const NumericType> tensorTypes,
                    std::span<const NumericType> scalarTypes,
                    std::span<const std::uint32_t> reads = {}) noexcept;
  std::expected<Kernel, Error> loadNumericReductionKernel(std::string_view, Device,
                                                          NumericType) noexcept;
  std::expected<Kernel, Error> loadNumericMatmulKernel(std::string_view, Device,
                                                       NumericType) noexcept;
  std::expected<Kernel, Error> loadNumericGraphKernel(
      std::string_view, Device, NumericType, ElementwiseSignature,
      std::span<const NumericType> tensorTypes, std::span<const NumericType> scalarTypes,
      std::span<const std::uint32_t> reads, std::span<const std::uint8_t> scalarBuffers) noexcept;
  std::expected<NumericExecutable, Error> prepareNumericProgram(std::uint32_t inputCount,
                                                                std::span<const NumericInstruction>,
                                                                std::uint32_t result, NumericType,
                                                                bool scalarResult) noexcept;
  TaskHandle executeNumericProgram(TaskCore &, GroupHandle, const NumericExecutable &,
                                   std::span<const Buffer>,
                                   std::span<const std::uint64_t> captures = {}) noexcept;
  TaskHandle executeNumericProgram(Task &, GroupHandle, const NumericExecutable &,
                                   std::span<const Buffer>,
                                   std::span<const std::uint64_t> captures = {}) noexcept;
  // Reusable synchronous caller for tensor results. Scalar executables use
  // their typed TaskEntry layout with ClosedTaskRunner::run instead.
  std::expected<Buffer, Failure>
  executeNumericProgram(ClosedTaskRunner &, const NumericExecutable &, std::span<const Buffer>,
                        std::span<const std::uint64_t> captures = {}) noexcept;
  TaskHandle numericProgram(TaskCore &, GroupHandle, std::span<const Buffer>,
                            std::span<const NumericInstruction>, std::uint32_t result, NumericType,
                            bool scalarResult) noexcept;
  TaskHandle numericProgram(Task &, GroupHandle, std::span<const Buffer>,
                            std::span<const NumericInstruction>, std::uint32_t result, NumericType,
                            bool scalarResult) noexcept;
  TaskHandle numericElementwise(TaskCore &, GroupHandle, Kernel, std::span<const Buffer>,
                                std::span<const std::uint64_t> scalarBits = {},
                                Kernel reduction = {}, bool scalarResult = false) noexcept;
  TaskHandle numericElementwise(Task &, GroupHandle, Kernel, std::span<const Buffer>,
                                std::span<const std::uint64_t> scalarBits = {},
                                Kernel reduction = {}, bool scalarResult = false) noexcept;
  std::expected<Kernel, Error> loadReductionKernel(std::string_view metallib, Device) noexcept;
  std::expected<Kernel, Error> loadMatmulKernel(std::string_view metallib, Device) noexcept;
  std::expected<Kernel, Error> loadTensorTransformKernel(std::string_view metallib, Device,
                                                         TensorTransformKind, NumericType,
                                                         bool deviceEpsilon = false) noexcept;
  // One task owns optional operand maps and a post stage. Map inputs are the
  // original immutable arguments; post slot zero selects the completed matrix.
  TaskHandle matmul(TaskCore &, GroupHandle, Kernel, Buffer, Buffer, Kernel post = {},
                    std::span<const Buffer> postInputs = {}, std::span<const float> scalars = {},
                    Kernel leftMap = {}, Kernel rightMap = {},
                    std::span<const Buffer> mapInputs = {},
                    std::span<const float> mapScalars = {}) noexcept;
  TaskHandle matmul(Task &, GroupHandle, Kernel, Buffer, Buffer, Kernel post = {},
                    std::span<const Buffer> postInputs = {}, std::span<const float> scalars = {},
                    Kernel leftMap = {}, Kernel rightMap = {},
                    std::span<const Buffer> mapInputs = {},
                    std::span<const float> mapScalars = {}) noexcept;
  // Copies the product graph and inputs; one task owns all resident
  // intermediates.
  TaskHandle matmulGraph(TaskCore &, GroupHandle, Kernel, std::span<const Buffer>,
                         std::span<const MatrixProduct>, std::span<const Kernel> maps = {},
                         std::span<const float> mapScalars = {}) noexcept;
  TaskHandle matmulGraph(Task &, GroupHandle, Kernel, std::span<const Buffer>,
                         std::span<const MatrixProduct>, std::span<const Kernel> maps = {},
                         std::span<const float> mapScalars = {}) noexcept;
  // Resident product graph followed by one strict reduction and optional scalar
  // epilogue.
  TaskHandle matmulGraphReduceSum(TaskCore &, GroupHandle, Kernel, std::span<const Buffer>,
                                  std::span<const MatrixProduct>, Kernel reduction,
                                  Kernel epilogue = {}, std::span<const float> scalars = {},
                                  std::span<const Kernel> maps = {},
                                  std::span<const float> mapScalars = {}) noexcept;
  TaskHandle matmulGraphReduceSum(Task &, GroupHandle, Kernel, std::span<const Buffer>,
                                  std::span<const MatrixProduct>, Kernel reduction,
                                  Kernel epilogue = {}, std::span<const float> scalars = {},
                                  std::span<const Kernel> maps = {},
                                  std::span<const float> mapScalars = {}) noexcept;
  // Tensor-result graph with resident reduction broadcasts and no result
  // readback.
  TaskHandle graphTensor(TaskCore &, GroupHandle, Kernel base, std::span<const Buffer>,
                         std::span<const MatrixProduct>, Kernel reduction,
                         std::span<const std::uint32_t> roots, std::span<const Kernel> maps,
                         std::span<const float> mapScalars,
                         std::span<const MatrixScalar> broadcasts) noexcept;
  TaskHandle graphTensor(Task &, GroupHandle, Kernel base, std::span<const Buffer>,
                         std::span<const MatrixProduct>, Kernel reduction,
                         std::span<const std::uint32_t> roots, std::span<const Kernel> maps,
                         std::span<const float> mapScalars,
                         std::span<const MatrixScalar> broadcasts) noexcept;
  // Resident reductions may feed later maps; only the final scalar expression
  // is read back.
  TaskHandle graphReductions(TaskCore &, GroupHandle, Kernel base, std::span<const Buffer>,
                             std::span<const MatrixProduct>, Kernel reduction, Kernel join,
                             std::span<const std::uint32_t> roots, std::span<const float> scalars,
                             std::span<const Kernel> maps = {},
                             std::span<const float> mapScalars = {},
                             std::span<const MatrixScalar> broadcasts = {}) noexcept;
  TaskHandle graphReductions(Task &, GroupHandle, Kernel base, std::span<const Buffer>,
                             std::span<const MatrixProduct>, Kernel reduction, Kernel join,
                             std::span<const std::uint32_t> roots, std::span<const float> scalars,
                             std::span<const Kernel> maps = {},
                             std::span<const float> mapScalars = {},
                             std::span<const MatrixScalar> broadcasts = {}) noexcept;
  // One scalar task: optional input maps, matrix, optional adjustment, fixed
  // reduction tree.
  TaskHandle matmulReduceSum(TaskCore &, GroupHandle, Kernel matrix, Kernel reduction, Buffer left,
                             Buffer right, Kernel leftMap = {}, Kernel rightMap = {},
                             std::span<const Buffer> mapInputs = {},
                             std::span<const float> mapScalars = {}, Kernel adjustment = {},
                             std::span<const Buffer> adjustmentInputs = {},
                             std::span<const float> adjustmentScalars = {}, Kernel epilogue = {},
                             std::span<const float> epilogueScalars = {}) noexcept;
  TaskHandle matmulReduceSum(Task &, GroupHandle, Kernel matrix, Kernel reduction, Buffer left,
                             Buffer right, Kernel leftMap = {}, Kernel rightMap = {},
                             std::span<const Buffer> mapInputs = {},
                             std::span<const float> mapScalars = {}, Kernel adjustment = {},
                             std::span<const Buffer> adjustmentInputs = {},
                             std::span<const float> adjustmentScalars = {}, Kernel epilogue = {},
                             std::span<const float> epilogueScalars = {}) noexcept;
  TaskHandle reduceSum(TaskCore &, GroupHandle, Kernel, Buffer, Kernel epilogue = {},
                       std::span<const float> scalars = {}) noexcept;
  TaskHandle reduceSum(Task &, GroupHandle, Kernel, Buffer, Kernel epilogue = {},
                       std::span<const float> scalars = {}) noexcept;
  // One scheduled scalar task; a resident elementwise stage precedes the fixed
  // reduction tree.
  TaskHandle elementwiseReduceSum(TaskCore &, GroupHandle, Kernel elementwise, Kernel reduction,
                                  std::span<const Buffer>, std::span<const float>,
                                  Kernel epilogue = {}) noexcept;
  TaskHandle elementwiseReduceSum(Task &, GroupHandle, Kernel elementwise, Kernel reduction,
                                  std::span<const Buffer>, std::span<const float>,
                                  Kernel epilogue = {}) noexcept;
  static const ValueLayout &scalarLayout() noexcept;
  TaskHandle elementwise(TaskCore &, GroupHandle, Kernel, std::span<const Buffer>,
                         std::span<const float>) noexcept;
  TaskHandle elementwise(Task &, GroupHandle, Kernel, std::span<const Buffer>,
                         std::span<const float>) noexcept;
  TaskHandle elementwise(TaskCore &, GroupHandle, Kernel, Buffer, Buffer) noexcept;
  TaskHandle elementwise(Task &, GroupHandle, Kernel, Buffer, Buffer) noexcept;
  TaskHandle compute(TaskCore &, GroupHandle, Kernel, BinaryOperation, Buffer, Buffer) noexcept;
  TaskHandle compute(Task &, GroupHandle, Kernel, BinaryOperation, Buffer, Buffer) noexcept;
  static const ValueLayout &bufferLayout() noexcept;
  Stats stats() const noexcept;
  // Private, synchronized test controls. Reset allocation failure only when
  // admission is quiescent; held completion must be released before shutdown.
  void failAllocationAt(std::uint64_t attempt) noexcept;
  void failNextCompletion() noexcept;
  void holdCompletions(bool hold) noexcept;
  void holdCompletionAt(std::uint64_t submission) noexcept;
  std::uint64_t progressionCommandReservations() const noexcept;

private:
  std::shared_ptr<State> state_;
  explicit Runtime(std::shared_ptr<State>) noexcept;
  std::expected<void *, Error> exportCpu(Invocation &, const Buffer &, NumericType,
                                         std::span<std::int64_t> sizes,
                                         std::span<std::int64_t> strides) noexcept;
  std::expected<Kernel, Error>
  loadKernel(std::string_view, Device, bool elementwise,
             std::optional<ElementwiseSignature> signature = {}, bool reduction = false,
             bool matrix = false, std::optional<std::span<const std::uint32_t>> reads = {},
             std::optional<NumericType> numericType = {},
             std::span<const NumericType> tensorTypes = {},
             std::span<const NumericType> scalarTypes = {},
             std::span<const std::uint8_t> scalarBuffers = {}) noexcept;
};
} // namespace kalandra::runtime::metal
