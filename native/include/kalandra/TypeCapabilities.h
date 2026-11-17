#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/Semantics.h"

namespace kalandra {
enum class TypeStorage { Scalar, Tensor, Array };

// A non-owning view of the current type. No cached facts, shape copies or
// normalization of malformed flags. The element tag is meaningful only for
// Numeric, even though other physical layouts also carry its default value.
class TypeFacts {
  const ValueType &value_;
  explicit TypeFacts(const ValueType &value) : value_(value) {}
  friend TypeFacts typeFacts(const ValueType &);

public:
  BuiltinValue builtin() const { return value_.builtin; }
  TypeStorage storage() const {
    return value_.array    ? TypeStorage::Array
           : value_.tensor ? TypeStorage::Tensor
                           : TypeStorage::Scalar;
  }
  bool tensor() const { return value_.tensor; }
  bool array() const { return value_.array; }
  std::size_t rank() const { return value_.shape.size(); }
  bool nominal() const { return value_.record.has_value(); }
  bool callableSignature() const { return bool(value_.callable); }
  bool errorFamily() const { return value_.errorRecord.has_value(); }
  bool resident() const { return value_.device.has_value(); }
  bool task() const { return value_.task; }
  bool fallible() const { return value_.fallible; }
  bool numeric() const { return value_.builtin == BuiltinValue::Numeric; }
  bool booleanStorage() const { return isBooleanStorage(value_); }
  std::optional<NumericFamily> numericFamily() const {
    if (numeric())
      if (auto description = describeNumeric(value_.element))
        return description->family;
    return {};
  }
  bool completed() const { return !value_.task && !value_.fallible; }
  bool completedHostValue() const { return completed() && !resident(); }
  bool plainHostValue() const { return completedHostValue() && !errorFamily(); }
  bool completedHostScalar() const {
    return completedHostValue() && !tensor() && !array() && rank() == 0;
  }
  bool plainHostScalar() const { return completedHostScalar() && !errorFamily(); }
};
inline TypeFacts typeFacts(const ValueType &value) { return TypeFacts(value); }
TypeFacts typeFacts(ValueType &&) = delete;
TypeFacts typeFacts(const ValueType &&) = delete;

// Source map signatures are checked before normalized IR storage validation.
// Preserve their established shape/error-metadata rules and parallel errors.
inline bool hostMapElementPrerequisite(const ValueType &type, bool parallel) {
  const auto facts = typeFacts(type);
  const bool element =
      facts.numeric() || facts.booleanStorage() || facts.builtin() == BuiltinValue::String ||
      facts.builtin() == BuiltinValue::Record || facts.builtin() == BuiltinValue::Function;
  return element && !facts.tensor() && !facts.task() && (!facts.fallible() || parallel) &&
         !facts.resident();
}

enum class CapabilityTarget { Cpu, Metal, Vulkan, Cuda };
constexpr bool hostExecutionAvailable(CapabilityTarget target) {
  return target == CapabilityTarget::Cpu;
}
// Legacy numeric IR is a validation profile, not a full GPU backend selector.
constexpr bool hostExecutionAvailable(NumericTarget target) { return target == NumericTarget::Cpu; }

enum class NumericCapabilityOperation {
  Storage,
  ScalarArithmetic,
  TensorElementwise,
  Reduction,
  Matmul,
  Conversion,
  HostArrayStorage
};
enum class CapabilityFailure {
  None,
  InvalidNumericType,
  InvalidTarget,
  InvalidOperation,
  InvalidArithmeticPolicy,
  HostStorageRequired,
  MetalHardwareFloat64
};
struct NumericCapability {
  CapabilityFailure failure = CapabilityFailure::None;
  // Static eligibility is never a certificate of actual driver/device support.
  bool requiresDeviceValidation = false;
  bool requiresNativeFloat64 = false;
  constexpr bool accepted() const { return failure == CapabilityFailure::None; }
};
constexpr NumericCapability numericTypeCapability(NumericType type, CapabilityTarget target,
                                                  ArithmeticMode arithmetic,
                                                  NumericCapabilityOperation operation) {
  if (!describeNumeric(type))
    return {CapabilityFailure::InvalidNumericType};
  switch (target) {
  case CapabilityTarget::Cpu:
  case CapabilityTarget::Metal:
  case CapabilityTarget::Vulkan:
  case CapabilityTarget::Cuda:
    break;
  default:
    return {CapabilityFailure::InvalidTarget};
  }
  switch (operation) {
  case NumericCapabilityOperation::Storage:
  case NumericCapabilityOperation::ScalarArithmetic:
  case NumericCapabilityOperation::TensorElementwise:
  case NumericCapabilityOperation::Reduction:
  case NumericCapabilityOperation::Matmul:
  case NumericCapabilityOperation::Conversion:
  case NumericCapabilityOperation::HostArrayStorage:
    break;
  default:
    return {CapabilityFailure::InvalidOperation};
  }
  switch (arithmetic) {
  case ArithmeticMode::Hardware:
  case ArithmeticMode::Software:
  case ArithmeticMode::HardwareWithSoftwareFallback:
    break;
  default:
    return {CapabilityFailure::InvalidArithmeticPolicy};
  }
  const bool device = !hostExecutionAvailable(target);
  if (operation == NumericCapabilityOperation::HostArrayStorage && device)
    return {CapabilityFailure::HostStorageRequired};
  const bool nativeDouble = type == NumericType::Float64 &&
                            operation != NumericCapabilityOperation::Storage &&
                            arithmetic == ArithmeticMode::Hardware && device;
  if (nativeDouble && target == CapabilityTarget::Metal)
    return {CapabilityFailure::MetalHardwareFloat64};
  return {CapabilityFailure::None, device, nativeDouble};
}
constexpr std::string_view capabilityFailureMessage(CapabilityFailure failure) {
  switch (failure) {
  case CapabilityFailure::None:
    return {};
  case CapabilityFailure::InvalidNumericType:
    return "numeric type is not implemented";
  case CapabilityFailure::InvalidTarget:
    return "invalid capability target";
  case CapabilityFailure::InvalidOperation:
    return "invalid numeric capability operation";
  case CapabilityFailure::InvalidArithmeticPolicy:
    return "invalid arithmetic policy";
  case CapabilityFailure::HostStorageRequired:
    return "host array storage requires CPU execution";
  case CapabilityFailure::MetalHardwareFloat64:
    return "Metal has no hardware f64 arithmetic; explicitly select "
           "--arithmetic hardware-with-software-fallback or software";
  }
  return "invalid capability failure";
}

// These are structural prerequisites for the shared GPU compiler. They
// deliberately retain existing checks; module/layout/provenance validation is
// separate. In particular, callable storage alone never proves transport safe.
enum class DeviceTypeStorage { Scalar, FlatArray, InlineRecord, RecordArray, Callable, Tensor };
inline bool deviceTypeStorage(const ValueType &type, DeviceTypeStorage storage) {
  const auto facts = typeFacts(type);
  if (!facts.completed())
    return false;
  switch (storage) {
  case DeviceTypeStorage::Scalar:
    return !facts.tensor() && !facts.array() && (facts.numeric() || facts.booleanStorage());
  case DeviceTypeStorage::FlatArray:
    return facts.array() && facts.tensor() && facts.rank() == 1 &&
           (facts.numeric() || facts.booleanStorage());
  case DeviceTypeStorage::InlineRecord:
    return facts.nominal() && facts.builtin() == BuiltinValue::Record && !facts.tensor() &&
           !facts.array();
  case DeviceTypeStorage::RecordArray:
    return isRecordArray(type) && facts.nominal() && facts.tensor() && facts.rank() == 1;
  case DeviceTypeStorage::Callable:
    return facts.builtin() == BuiltinValue::Function && !facts.array() && !facts.tensor();
  case DeviceTypeStorage::Tensor:
    return facts.tensor() && !facts.array() && facts.numeric() && facts.rank() <= 8;
  }
  return false;
}
} // namespace kalandra
