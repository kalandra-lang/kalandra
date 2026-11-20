#include "../gpu-source/AggregateLayout.h"
#include "../gpu-source/Supervisor.h"
#include "kalandra/TypeCapabilities.h"
#include <array>
#include <stdexcept>
#include <utility>
using namespace kalandra;
namespace {
void expect(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
concept FactViewable = requires(T &&value) { typeFacts(std::forward<T>(value)); };
static_assert(FactViewable<ValueType &> && FactViewable<const ValueType &>);
static_assert(!FactViewable<ValueType> && !FactViewable<const ValueType>);

// Frozen pre-migration predicates: deliberately independent of the new facts.
// They cover invalid IR states too, so a shared query cannot silently normalize
// an array flag, error family or nominal identity into different eligibility.
bool legacyDevice(const ValueType &t, DeviceTypeStorage storage) {
  const bool boolean = t.builtin == BuiltinValue::Bool || t.builtin == BuiltinValue::Unit;
  switch (storage) {
  case DeviceTypeStorage::Scalar:
    return !t.tensor && !t.array && !t.task && !t.fallible &&
           (t.builtin == BuiltinValue::Numeric || boolean);
  case DeviceTypeStorage::FlatArray:
    return t.array && t.tensor && t.shape.size() == 1 && !t.task && !t.fallible &&
           (t.builtin == BuiltinValue::Numeric || boolean);
  case DeviceTypeStorage::InlineRecord:
    return t.record && t.builtin == BuiltinValue::Record && !t.tensor && !t.array && !t.task &&
           !t.fallible;
  case DeviceTypeStorage::RecordArray:
    return t.builtin == BuiltinValue::Record && t.array && t.record && t.tensor &&
           t.shape.size() == 1 && !t.task && !t.fallible;
  case DeviceTypeStorage::Callable:
    return t.builtin == BuiltinValue::Function && !t.array && !t.tensor && !t.task && !t.fallible;
  case DeviceTypeStorage::Tensor:
    return t.tensor && !t.array && !t.task && !t.fallible && t.builtin == BuiltinValue::Numeric &&
           t.shape.size() <= 8;
  }
  return false;
}
constexpr std::array storages{DeviceTypeStorage::Scalar,       DeviceTypeStorage::FlatArray,
                              DeviceTypeStorage::InlineRecord, DeviceTypeStorage::RecordArray,
                              DeviceTypeStorage::Callable,     DeviceTypeStorage::Tensor};
} // namespace
void checkTypeCapabilities() {
  const auto callable = std::make_shared<const FunctionSignature>();
  for (unsigned builtin = 0; builtin <= unsigned(BuiltinValue::Unit); ++builtin)
    for (unsigned numeric = 0; numeric <= numericTypes.size(); ++numeric)
      for (std::size_t rank : {0u, 1u, 3u, 8u, 9u})
        for (unsigned flags = 0; flags < 256; ++flags) {
          ValueType t;
          t.builtin = static_cast<BuiltinValue>(builtin);
          t.element = numeric < numericTypes.size() ? numericTypes[numeric].type
                                                    : static_cast<NumericType>(255);
          t.tensor = flags & 1;
          t.array = flags & 2;
          t.task = flags & 4;
          t.fallible = flags & 8;
          if (flags & 16)
            t.errorRecord = 999;
          if (flags & 32)
            t.device = 0;
          if (flags & 64)
            t.record = 999;
          if (flags & 128)
            t.callable = callable;
          t.shape.resize(rank);
          const auto facts = typeFacts(t);
          const bool host = !t.task && !t.fallible && !t.device;
          const bool scalar = host && !t.array && !t.tensor && t.shape.empty();
          expect(facts.completedHostValue() == host &&
                     facts.plainHostValue() == (host && !t.errorRecord) &&
                     facts.completedHostScalar() == scalar &&
                     facts.plainHostScalar() == (scalar && !t.errorRecord),
                 "completed/plain host prerequisites changed");
          expect(facts.storage() == (t.array    ? TypeStorage::Array
                                     : t.tensor ? TypeStorage::Tensor
                                                : TypeStorage::Scalar) &&
                     facts.tensor() == t.tensor && facts.array() == t.array &&
                     facts.rank() == rank && facts.nominal() == bool(t.record) &&
                     facts.callableSignature() == bool(t.callable),
                 "type facts normalized storage or nominal/callable identity");
          const auto description = describeNumeric(t.element);
          const auto family = t.builtin == BuiltinValue::Numeric && description
                                  ? std::optional(description->family)
                                  : std::nullopt;
          expect(facts.numericFamily() == family, "nonnumeric/default or invalid numeric family");
          for (const auto storage : storages)
            expect(deviceTypeStorage(t, storage) == legacyDevice(t, storage),
                   "shared device prerequisite changed its legacy truth table");
          expect(deviceScalarValue(t) == legacyDevice(t, DeviceTypeStorage::Scalar) &&
                     deviceFlatArrayValue(t) == legacyDevice(t, DeviceTypeStorage::FlatArray) &&
                     deviceInlineRecordValue(t) ==
                         legacyDevice(t, DeviceTypeStorage::InlineRecord) &&
                     deviceRecordArrayValue(t) == legacyDevice(t, DeviceTypeStorage::RecordArray) &&
                     deviceCallableValue(t) == legacyDevice(t, DeviceTypeStorage::Callable) &&
                     deviceTensorValue(t) == legacyDevice(t, DeviceTypeStorage::Tensor),
                 "GPU layout adapter changed the shared predicate");
          for (bool parallel : {false, true}) {
            const bool element =
                t.builtin == BuiltinValue::Numeric || t.builtin == BuiltinValue::Bool ||
                t.builtin == BuiltinValue::Unit || t.builtin == BuiltinValue::Record ||
                t.builtin == BuiltinValue::String || t.builtin == BuiltinValue::Function;
            const bool legacy =
                element && !t.tensor && !t.task && (!t.fallible || parallel) && !t.device;
            expect(hostMapElementPrerequisite(t, parallel) == legacy,
                   "frontend map signature prerequisite changed");
          }
        }

  ValueType changing;
  const auto current = typeFacts(changing);
  changing.builtin = BuiltinValue::String;
  changing.array = true;
  expect(!current.numeric() && current.array() && !current.numericFamily(),
         "type facts cached stale state");

  TypedModule module;
  for (unsigned kind = 0; kind <= unsigned(StandardRecord::Captured); ++kind) {
    TypedRecord record;
    record.name = "File"; // Spelling is never conformance/resource identity.
    record.standard = static_cast<StandardRecord>(kind);
    module.records.push_back(record);
  }
  module.records.push_back(TypedRecord{});
  module.records.back().name = "File";
  for (std::size_t id = 0; id <= module.records.size(); ++id) {
    ValueType type;
    type.record = id;
    const auto standard = id < module.records.size() ? module.records[id].standard : std::nullopt;
    const bool file = standard == StandardRecord::File ||
                      standard == StandardRecord::BufferedReader ||
                      standard == StandardRecord::BufferedWriter || standard == StandardRecord::Child ||
                      standard == StandardRecord::PipeReader || standard == StandardRecord::PipeWriter;
    const bool buffer = standard == StandardRecord::ByteBuffer;
    const bool iterator = standard == StandardRecord::Iterator;
    expect(isFileResource(module, type) == file && isByteBuffer(module, type) == buffer &&
               isIterator(module, type) == iterator &&
               isOwnedResource(module, type) == (file || buffer || iterator),
           "canonical resource identity changed or inferred from spelling");
    type.array = type.tensor = type.task = type.fallible = true;
    expect(isOwnedResource(module, type) == (file || buffer || iterator),
           "resource identity fact incorrectly became storage validation");
  }

  const std::array targets{CapabilityTarget::Cpu, CapabilityTarget::Metal, CapabilityTarget::Vulkan,
                           CapabilityTarget::Cuda};
  const std::array modes{ArithmeticMode::Hardware, ArithmeticMode::Software,
                         ArithmeticMode::HardwareWithSoftwareFallback};
  for (const auto target : targets)
    for (const auto mode : modes)
      for (unsigned op = 0; op <= unsigned(NumericCapabilityOperation::HostArrayStorage); ++op)
        for (const auto description : numericTypes) {
          const auto operation = static_cast<NumericCapabilityOperation>(op);
          const auto result = numericTypeCapability(description.type, target, mode, operation);
          const bool device = target != CapabilityTarget::Cpu;
          const bool doubleArithmetic = description.type == NumericType::Float64 &&
                                        operation != NumericCapabilityOperation::Storage &&
                                        mode == ArithmeticMode::Hardware && device;
          const bool hostOnly = operation == NumericCapabilityOperation::HostArrayStorage && device;
          const bool metalDouble =
              !hostOnly && doubleArithmetic && target == CapabilityTarget::Metal;
          expect(result.accepted() == (!hostOnly && !metalDouble),
                 "numeric target/operation/policy matrix changed");
          expect(result.failure == (hostOnly      ? CapabilityFailure::HostStorageRequired
                                    : metalDouble ? CapabilityFailure::MetalHardwareFloat64
                                                  : CapabilityFailure::None),
                 "numeric capability failure reason changed");
          if (result.accepted())
            expect(result.requiresDeviceValidation == device &&
                       result.requiresNativeFloat64 == doubleArithmetic,
                   "static capability falsely certified an unknown device");
        }
  expect(!numericTypeCapability(static_cast<NumericType>(255), CapabilityTarget::Cpu,
                                ArithmeticMode::Hardware, NumericCapabilityOperation::Storage)
                 .accepted() &&
             !numericTypeCapability(NumericType::Float32, static_cast<CapabilityTarget>(255),
                                    ArithmeticMode::Hardware, NumericCapabilityOperation::Storage)
                  .accepted() &&
             !numericTypeCapability(NumericType::Float32, CapabilityTarget::Cpu,
                                    static_cast<ArithmeticMode>(255),
                                    NumericCapabilityOperation::Storage)
                  .accepted() &&
             !numericTypeCapability(NumericType::Float32, CapabilityTarget::Cpu,
                                    ArithmeticMode::Hardware,
                                    static_cast<NumericCapabilityOperation>(255))
                  .accepted(),
         "invalid capability request accepted");
  expect(capabilityTarget(GpuBackend::Metal) == CapabilityTarget::Metal &&
             capabilityTarget(GpuBackend::Vulkan) == CapabilityTarget::Vulkan &&
             capabilityTarget(GpuBackend::Cuda) == CapabilityTarget::Cuda &&
             !hostExecutionAvailable(static_cast<NumericTarget>(255)),
         "backend adapter changed target identity");
}
