#pragma once
#include "Async.h"
namespace kalandra::runtime::vulkan::language {
using Error = vulkan::Error;
using Format = NumericType;
using Buffer = TensorValue;
using NumericInstructionKind = NumericProgramKind;
enum class ElementwiseBinding { Direct, ArgumentBuffer };
struct ElementwiseSignature {
  std::uint32_t tensors = 1, scalars = 0;
  ElementwiseBinding binding = ElementwiseBinding::Direct;
};
constexpr std::optional<Format> storageFormat(NumericType type) {
  return describeNumeric(type) ? std::optional(type) : std::nullopt;
}
struct Registry;
class Device {
public:
  Device() = default;
  explicit operator bool() const noexcept { return bool(owner_); }
  std::string_view name() const noexcept;
  std::uint64_t identity() const noexcept;

private:
  std::shared_ptr<Registry> owner_;
  std::uint32_t index_ = 0;
  friend class Runtime;
  friend class NumericExecutable;
};
class Kernel {
public:
  Kernel() = default;
  explicit operator bool() const noexcept { return bool(schema_.kernel); }

private:
  ProgramKernel schema_{};
  Device device_;
  NumericInstructionKind kind_ = NumericInstructionKind::Map;
  friend class Runtime;
};
struct NumericInstruction {
  NumericInstructionKind kind;
  Kernel kernel{};
  std::span<const std::uint32_t> inputs{};
  std::span<const std::uint64_t> scalars{};
  std::uint32_t output = UINT32_MAX, target = 0, workPerElement = 0;
};
struct Prepared;
class NumericExecutable {
public:
  NumericExecutable() = default;
  explicit operator bool() const noexcept { return bool(plan_); }
  bool on(Device) const noexcept;

private:
  std::shared_ptr<const Prepared> plan_;
  friend class Runtime;
};
struct Options {
  bool validation = false;
  std::uint64_t maxBytes = 64 * 1024 * 1024;
  std::uint64_t maxArgumentBytes = 1024 * 1024;
  std::uint32_t maxInFlight = 64, defaultDevice = 0;
};
struct Stats {
  std::uint64_t bytes = 0, inFlight = 0, argumentBytes = 0;
  std::uint32_t initializedDevices = 0;
};
// Shared source-supervisor adapter. Private until CLI/distribution qualification.
class Runtime {
public:
  static std::expected<std::unique_ptr<Runtime>, Error> create(Options = {}) noexcept;
  ~Runtime();
  std::uint32_t deviceCount() const noexcept;
  std::expected<Device, Error> device() const noexcept;
  std::expected<Device, Error> device(std::uint32_t) const noexcept;
  std::expected<Buffer, Error> fromCpu(const Invocation &, CpuTensorView) noexcept;
  template <NumericType Element, std::size_t Rank>
  std::expected<CpuTensor<Element, Rank>, Error> toCpuTyped(Invocation &context,
                                                            const Buffer &value) noexcept {
    auto valid = validateHost(value);
    if (!valid)
      return std::unexpected(valid.error());
    return vulkan::toCpuTyped<Element, Rank>(context, value);
  }
  TaskHandle upload(TaskCore &, GroupHandle, Buffer, Device) noexcept;
  TaskHandle upload(Task &, GroupHandle, Buffer, Device) noexcept;
  TaskHandle download(TaskCore &, GroupHandle, Buffer) noexcept;
  TaskHandle download(Task &, GroupHandle, Buffer) noexcept;
  std::expected<Kernel, Error> loadNumericGraphKernel(std::string_view, Device, NumericType,
                                                      ElementwiseSignature,
                                                      std::span<const NumericType>,
                                                      std::span<const NumericType>,
                                                      std::span<const std::uint32_t>,
                                                      std::span<const std::uint8_t>) noexcept;
  std::expected<Kernel, Error> loadNumericReductionKernel(std::string_view, Device,
                                                          NumericType) noexcept;
  std::expected<Kernel, Error> loadNumericMatmulKernel(std::string_view, Device,
                                                       NumericType) noexcept;
  std::expected<Kernel, Error> loadTensorTransformKernel(std::string_view, Device,
                                                         TensorTransformKind, NumericType,
                                                         bool deviceEpsilon = false) noexcept;
  std::expected<NumericExecutable, Error> prepareNumericProgram(std::uint32_t,
                                                                std::span<const NumericInstruction>,
                                                                std::uint32_t, NumericType, bool,
                                                                Device = {}) noexcept;
  TaskHandle executeNumericProgram(TaskCore &, GroupHandle, const NumericExecutable &,
                                   std::span<const Buffer>,
                                   std::span<const std::uint64_t> = {}) noexcept;
  TaskHandle executeNumericProgram(Task &, GroupHandle, const NumericExecutable &,
                                   std::span<const Buffer>,
                                   std::span<const std::uint64_t> = {}) noexcept;
  static const ValueLayout &bufferLayout() noexcept { return AsyncRuntime::tensorLayout(); }
  Stats stats() const noexcept;
  // Private deterministic acknowledgement controls, as in the lower adapter.
  void holdCompletions(bool) noexcept;

private:
  std::shared_ptr<Registry> state_;
  explicit Runtime(std::shared_ptr<Registry> state) : state_(std::move(state)) {}
  std::expected<Kernel, Error> load(std::string_view, Device, ProgramKernel,
                                    NumericInstructionKind) noexcept;
  std::expected<void, Error> validateHost(const Buffer &) const noexcept;
  template <class Caller> TaskHandle transfer(Caller &, GroupHandle, Buffer, Device, bool) noexcept;
  template <class Caller>
  TaskHandle execute(Caller &, GroupHandle, const NumericExecutable &, std::span<const Buffer>,
                     std::span<const std::uint64_t>) noexcept;
};
} // namespace kalandra::runtime::vulkan::language
