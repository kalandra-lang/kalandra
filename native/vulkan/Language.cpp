#include "Language.h"
#include <algorithm>
#include <new>
namespace kalandra::runtime::vulkan::language {
struct Registry {
  struct Context {
    std::shared_ptr<vulkan::Runtime> driver;
    std::unique_ptr<AsyncRuntime> async;
  };
  Options options;
  std::vector<DeviceInfo> devices;
  std::vector<Context> contexts;
  std::shared_ptr<ByteBudget> bytes, metadata, pending;
};
struct Prepared {
  std::shared_ptr<const HostCharge> charge;
  std::shared_ptr<Registry> owner;
  Program program;
  std::vector<std::uint64_t> captures;
  std::uint32_t device = 0;
};
std::string_view Device::name() const noexcept {
  return owner_ && index_ < owner_->devices.size() ? std::string_view(owner_->devices[index_].name)
                                                   : std::string_view{};
}
std::uint64_t Device::identity() const noexcept {
  // Opaque within a registry; equality/admission also checks its retained owner.
  return owner_ ? std::uint64_t(index_) + 1 : 0;
}
bool NumericExecutable::on(Device device) const noexcept {
  return plan_ && plan_->owner == device.owner_ && plan_->device == device.index_;
}
std::expected<std::unique_ptr<Runtime>, Error> Runtime::create(Options options) noexcept {
  if (!options.maxInFlight || options.maxInFlight > 4096)
    return std::unexpected(Error::Invalid);
  try {
    auto state = std::make_shared<Registry>();
    state->options = options;
    auto devices = vulkan::Runtime::enumerate(options.validation);
    if (!devices)
      return std::unexpected(devices.error());
    if (options.defaultDevice >= devices->size())
      return std::unexpected(Error::Unavailable);
    state->devices = std::move(*devices);
    auto bytes = vulkan::Runtime::createByteBudget(options.maxBytes);
    auto metadata = vulkan::Runtime::createByteBudget(options.maxArgumentBytes);
    auto pending = vulkan::Runtime::createByteBudget(options.maxInFlight);
    if (!bytes || !metadata || !pending)
      return std::unexpected(!bytes      ? bytes.error()
                             : !metadata ? metadata.error()
                                         : pending.error());
    state->bytes = std::move(*bytes);
    state->metadata = std::move(*metadata);
    state->pending = std::move(*pending);
    for (std::uint32_t i = 0; i < state->devices.size(); ++i) {
      auto made = vulkan::Runtime::create({.validation = options.validation,
                                           .deviceIndex = i,
                                           .budget = state->bytes,
                                           .deviceUuid = state->devices[i].uuid});
      if (!made)
        return std::unexpected(made.error());
      Registry::Context context;
      context.driver = std::move(*made);
      context.async =
          AsyncRuntime::create(context.driver, {.maxPending = options.maxInFlight,
                                                .maxCaptureBytes = options.maxArgumentBytes,
                                                .metadataBudget = state->metadata,
                                                .pendingBudget = state->pending});
      if (!context.async)
        return std::unexpected(Error::Allocation);
      state->contexts.push_back(std::move(context));
    }
    return std::unique_ptr<Runtime>(new Runtime(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
Runtime::~Runtime() = default;
std::uint32_t Runtime::deviceCount() const noexcept { return state_->contexts.size(); }
std::expected<Device, Error> Runtime::device() const noexcept {
  return device(state_->options.defaultDevice);
}
std::expected<Device, Error> Runtime::device(std::uint32_t index) const noexcept {
  if (index >= state_->contexts.size())
    return std::unexpected(Error::Unavailable);
  Device result;
  result.owner_ = state_;
  result.index_ = index;
  return result;
}
std::expected<Buffer, Error> Runtime::fromCpu(const Invocation &context,
                                              CpuTensorView view) noexcept {
  return vulkan::fromCpu(*state_->contexts[state_->options.defaultDevice].driver, context, view);
}
std::expected<void, Error> Runtime::validateHost(const Buffer &value) const noexcept {
  return validateTransfer(*state_->contexts[state_->options.defaultDevice].driver, value, true);
}
template <class Caller>
TaskHandle Runtime::transfer(Caller &caller, GroupHandle group, Buffer value, Device device,
                             bool upload) noexcept {
  auto &fallback = *state_->contexts[state_->options.defaultDevice].async;
  if (upload) {
    if (device.owner_ != state_ || device.index_ >= state_->contexts.size())
      return fallback.reject(caller, group, Error::Residency);
    return state_->contexts[device.index_].async->transfer(caller, group, value, true);
  }
  for (auto &context : state_->contexts)
    if (context.driver->owns(value.data))
      return context.async->transfer(caller, group, value, false);
  return fallback.reject(caller, group, Error::Residency);
}
TaskHandle Runtime::upload(TaskCore &core, GroupHandle group, Buffer value,
                           Device device) noexcept {
  return transfer(core, group, std::move(value), std::move(device), true);
}
TaskHandle Runtime::upload(Task &task, GroupHandle group, Buffer value, Device device) noexcept {
  return transfer(task, group, std::move(value), std::move(device), true);
}
TaskHandle Runtime::download(TaskCore &core, GroupHandle group, Buffer value) noexcept {
  return transfer(core, group, std::move(value), {}, false);
}
TaskHandle Runtime::download(Task &task, GroupHandle group, Buffer value) noexcept {
  return transfer(task, group, std::move(value), {}, false);
}
std::expected<Kernel, Error> Runtime::load(std::string_view path, Device device,
                                           ProgramKernel schema,
                                           NumericInstructionKind kind) noexcept {
  if (device.owner_ != state_ || device.index_ >= state_->contexts.size())
    return std::unexpected(Error::Residency);
  try {
    auto &driver = *state_->contexts[device.index_].driver;
    auto loaded = driver.loadKernel(std::filesystem::path(path));
    if (!loaded)
      return std::unexpected(loaded.error());
    schema.kernel = std::move(*loaded);
    Kernel result;
    result.schema_ = std::move(schema);
    result.device_ = std::move(device);
    result.kind_ = kind;
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Kernel, Error> Runtime::loadNumericGraphKernel(
    std::string_view path, Device device, NumericType type, ElementwiseSignature signature,
    std::span<const NumericType> tensors, std::span<const NumericType> scalars,
    std::span<const std::uint32_t> reads, std::span<const std::uint8_t> roles) noexcept {
  if (signature.tensors != tensors.size() || signature.scalars != scalars.size() ||
      roles.size() != tensors.size())
    return std::unexpected(Error::Invalid);
  try {
    ProgramKernel schema;
    schema.type = type;
    schema.tensorTypes.assign(tensors.begin(), tensors.end());
    schema.scalarTypes.assign(scalars.begin(), scalars.end());
    schema.scalarBuffers.assign(roles.begin(), roles.end());
    schema.reads.emplace(reads.begin(), reads.end());
    schema.scalar = std::none_of(roles.begin(), roles.end(), [](auto role) { return role == 0; });
    return load(path, std::move(device), std::move(schema), NumericInstructionKind::Map);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<Kernel, Error> Runtime::loadNumericReductionKernel(std::string_view path,
                                                                 Device device,
                                                                 NumericType type) noexcept {
  ProgramKernel schema;
  schema.type = type;
  return load(path, std::move(device), std::move(schema), NumericInstructionKind::Reduce);
}
std::expected<Kernel, Error> Runtime::loadNumericMatmulKernel(std::string_view path, Device device,
                                                              NumericType type) noexcept {
  ProgramKernel schema;
  schema.type = type;
  return load(path, std::move(device), std::move(schema), NumericInstructionKind::Matmul);
}
std::expected<Kernel, Error>
Runtime::loadTensorTransformKernel(std::string_view path, Device device, TensorTransformKind kind,
                                   NumericType type, bool deviceEpsilon) noexcept {
  if (!describeNumeric(type))
    return std::unexpected(Error::Invalid);
  if (kind != TensorTransformKind::Transpose && type != NumericType::Float32)
    return std::unexpected(Error::Unsupported);
  if ((kind != TensorTransformKind::RmsNormRows && deviceEpsilon) ||
      (kind != TensorTransformKind::Transpose && kind != TensorTransformKind::SoftmaxRows &&
       kind != TensorTransformKind::RmsNormRows))
    return std::unexpected(Error::Invalid);
  try {
    ProgramKernel schema;
    schema.type = type;
    schema.tensorTypes = {type};
    schema.scalarBuffers = {0};
    schema.transform = kind;
    if (kind == TensorTransformKind::RmsNormRows) {
      if (deviceEpsilon) {
        schema.tensorTypes.push_back(type);
        schema.scalarBuffers.push_back(1);
      } else
        schema.scalarTypes = {type};
    }
    return load(path, std::move(device), std::move(schema), NumericInstructionKind::Map);
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
std::expected<NumericExecutable, Error>
Runtime::prepareNumericProgram(std::uint32_t inputs, std::span<const NumericInstruction> code,
                               std::uint32_t result, NumericType type, bool scalar,
                               Device selected) noexcept {
  try {
    std::uint64_t bytes = sizeof(Prepared) + sizeof(ProgramExecution), slots = inputs;
    const auto maximum = state_->options.maxArgumentBytes;
    auto charge = [&](std::uint64_t count, std::uint64_t width) {
      if (bytes > maximum || count > (maximum - bytes) / width)
        return false;
      bytes += count * width;
      return true;
    };
    if (!charge(code.size(), 2 * (sizeof(ProgramInstruction) + sizeof(ProgramKernel))))
      return std::unexpected(Error::Budget);
    for (const auto &op : code) {
      if (op.output != UINT32_MAX)
        slots = std::max(slots, std::uint64_t(op.output) + 1);
      if (!charge(op.inputs.size(), 2 * sizeof(std::uint32_t)) ||
          !charge(op.scalars.size(), 2 * sizeof(std::uint64_t) + sizeof(std::uint32_t)))
        return std::unexpected(Error::Budget);
      if (op.kernel) {
        const auto &schema = op.kernel.schema_;
        if (!charge(schema.tensorTypes.size() + schema.scalarTypes.size(),
                    2 * sizeof(NumericType)) ||
            !charge(schema.scalarBuffers.size(), 2 * sizeof(std::uint8_t)) ||
            (schema.reads && !charge(schema.reads->size(), 2 * sizeof(std::uint32_t))))
          return std::unexpected(Error::Budget);
        if (!selected)
          selected = op.kernel.device_;
        if (op.kernel.device_.owner_ != state_ || op.kernel.device_.index_ != selected.index_ ||
            op.kernel.kind_ != op.kind)
          return std::unexpected(Error::Residency);
      }
    }
    if (!selected)
      selected = *device();
    if (selected.owner_ != state_ || selected.index_ >= state_->contexts.size())
      return std::unexpected(Error::Residency);
    if (!charge(slots, sizeof(TensorValue) + sizeof(std::uint32_t)))
      return std::unexpected(Error::Budget);
    auto reserved = vulkan::Runtime::reserveBytes(state_->metadata, bytes);
    if (!reserved)
      return std::unexpected(reserved.error());
    auto plan = std::make_shared<Prepared>();
    plan->charge = std::move(*reserved);
    plan->owner = state_;
    plan->device = selected.index_;
    std::vector<ProgramInstruction> owned;
    owned.reserve(code.size());
    std::size_t captureCount = 0;
    for (const auto &op : code)
      if (op.kind == NumericInstructionKind::Map)
        captureCount += op.scalars.size();
    plan->captures.reserve(captureCount);
    for (const auto &op : code) {
      ProgramInstruction instruction{op.kind, {}, {}, {}, {}};
      instruction.inputs.assign(op.inputs.begin(), op.inputs.end());
      instruction.output = op.output;
      instruction.target = op.target;
      if (op.kernel)
        instruction.kernel = op.kernel.schema_;
      if (op.kind == NumericInstructionKind::Map) {
        for (auto word : op.scalars) {
          instruction.scalarSources.push_back(plan->captures.size());
          plan->captures.push_back(word);
        }
      } else
        instruction.constants.assign(op.scalars.begin(), op.scalars.end());
      owned.push_back(std::move(instruction));
    }
    auto program = Program::prepare(*state_->contexts[plan->device].driver, inputs, owned, result,
                                    type, scalar, {.maxMetadataBytes = maximum});
    if (!program)
      return std::unexpected(program.error());
    plan->program = std::move(*program);
    NumericExecutable executable;
    executable.plan_ = std::move(plan);
    return executable;
  } catch (const std::bad_alloc &) {
    return std::unexpected(Error::Allocation);
  }
}
template <class Caller>
TaskHandle Runtime::execute(Caller &caller, GroupHandle group, const NumericExecutable &executable,
                            std::span<const Buffer> inputs,
                            std::span<const std::uint64_t> captures) noexcept {
  auto &fallback = *state_->contexts[state_->options.defaultDevice].async;
  if (!executable.plan_)
    return fallback.reject(caller, group, Error::Invalid);
  const auto &plan = *executable.plan_;
  if (plan.owner != state_)
    return fallback.reject(caller, group, Error::Residency, plan.program.scalarType());
  if (!captures.empty() && captures.size() != plan.captures.size())
    return fallback.reject(caller, group, Error::Invalid, plan.program.scalarType());
  return state_->contexts[plan.device].async->executeProgram(
      caller, group, plan.program, inputs,
      captures.empty() ? std::span<const std::uint64_t>(plan.captures) : captures);
}
TaskHandle Runtime::executeNumericProgram(TaskCore &core, GroupHandle group,
                                          const NumericExecutable &plan,
                                          std::span<const Buffer> inputs,
                                          std::span<const std::uint64_t> captures) noexcept {
  return execute(core, group, plan, inputs, captures);
}
TaskHandle Runtime::executeNumericProgram(Task &task, GroupHandle group,
                                          const NumericExecutable &plan,
                                          std::span<const Buffer> inputs,
                                          std::span<const std::uint64_t> captures) noexcept {
  return execute(task, group, plan, inputs, captures);
}
Stats Runtime::stats() const noexcept {
  return {vulkan::Runtime::reservedBytes(state_->bytes),
          vulkan::Runtime::reservedBytes(state_->pending),
          vulkan::Runtime::reservedBytes(state_->metadata)};
}
void Runtime::holdCompletions(bool hold) noexcept {
  for (auto &context : state_->contexts)
    context.async->holdCompletions(hold);
}
} // namespace kalandra::runtime::vulkan::language
