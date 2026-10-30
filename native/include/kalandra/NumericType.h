#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace kalandra {
// Concrete core types, independent of aliases, host C++ types and target ABI.
// This internal identity is not a serialized contract/versioned ABI identifier.
enum class NumericType {
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float32,
  Float64
};
enum class NumericFamily { SignedInteger, UnsignedInteger, BinaryFloat };
struct NumericDescription {
  NumericType type;
  std::string_view name;
  NumericFamily family;
  unsigned bits;
  constexpr unsigned bytes() const { return bits / 8; }
};
inline constexpr std::array numericTypes{
    NumericDescription{NumericType::Int8, "int8", NumericFamily::SignedInteger, 8},
    NumericDescription{NumericType::Int16, "int16", NumericFamily::SignedInteger, 16},
    NumericDescription{NumericType::Int32, "int32", NumericFamily::SignedInteger, 32},
    NumericDescription{NumericType::Int64, "int64", NumericFamily::SignedInteger, 64},
    NumericDescription{NumericType::UInt8, "uint8", NumericFamily::UnsignedInteger, 8},
    NumericDescription{NumericType::UInt16, "uint16", NumericFamily::UnsignedInteger, 16},
    NumericDescription{NumericType::UInt32, "uint32", NumericFamily::UnsignedInteger, 32},
    NumericDescription{NumericType::UInt64, "uint64", NumericFamily::UnsignedInteger, 64},
    NumericDescription{NumericType::Float32, "float32", NumericFamily::BinaryFloat, 32},
    NumericDescription{NumericType::Float64, "float64", NumericFamily::BinaryFloat, 64}};
constexpr std::optional<NumericDescription> describeNumeric(NumericType type) {
  for (auto description : numericTypes)
    if (description.type == type)
      return description;
  return {};
}
constexpr std::optional<NumericType> resolveNumeric(std::string_view name) {
  if (name == "int")
    return NumericType::Int64;
  if (name == "uint")
    return NumericType::UInt64;
  if (name == "float")
    return NumericType::Float64;
  for (auto description : numericTypes)
    if (description.name == name)
      return description.type;
  return {};
}
constexpr bool isInteger32(NumericType type) {
  return type == NumericType::Int32 || type == NumericType::UInt32;
}
constexpr bool isUnsigned(NumericType type) {
  auto description = describeNumeric(type);
  return description && description->family == NumericFamily::UnsignedInteger;
}
constexpr bool isInteger(NumericType type) {
  auto description = describeNumeric(type);
  return description && description->family != NumericFamily::BinaryFloat;
}
// Type-level availability is necessary but does not promise every operation,
// rank or device capability. Those checks remain with the corresponding backend.
enum class NumericTarget { Cpu, AppleGpu };
constexpr bool numericArithmeticAvailable(NumericType type, NumericTarget target) {
  switch (target) {
  case NumericTarget::Cpu:
    return type == NumericType::Float32 || type == NumericType::Float64 || isInteger32(type) ||
           type == NumericType::Int64 || type == NumericType::UInt64;
  case NumericTarget::AppleGpu:
    return describeNumeric(type).has_value();
  }
  return false;
}
constexpr bool isInteger8(NumericType type) {
  return type == NumericType::Int8 || type == NumericType::UInt8;
}
constexpr bool isNarrowInteger(NumericType type) {
  return isInteger8(type) || type == NumericType::Int16 || type == NumericType::UInt16;
}
constexpr bool numericScalarArithmeticAvailable(NumericType type, NumericTarget target) {
  return numericArithmeticAvailable(type, target) ||
         (target == NumericTarget::Cpu && isNarrowInteger(type));
}
// Tensor construction and elementwise arithmetic have a distinct capability
// from reductions, matmul and conversions, which retain their own gates.
constexpr bool numericTensorElementwiseAvailable(NumericType type, NumericTarget target) {
  return numericScalarArithmeticAvailable(type, target);
}
// Reductions preserve the element width and check each integer tree addition.
constexpr bool numericReductionAvailable(NumericType type, NumericTarget target) {
  return numericTensorElementwiseAvailable(type, target);
}
// Matmul checks each product before reducing it at the same element width.
constexpr bool numericMatmulAvailable(NumericType type, NumericTarget target) {
  return numericTensorElementwiseAvailable(type, target);
}
// Tensor casts preserve checked scalar conversion semantics lane by lane.
constexpr bool numericTensorConversionAvailable(NumericType type, NumericTarget target) {
  return numericScalarArithmeticAvailable(type, target);
}
// Collection storage is separate from tensor arithmetic capability. CPU arrays
// can retain every implemented scalar width without enabling tensor operations.
constexpr bool numericCollectionStorageAvailable(NumericType type, NumericTarget target) {
  return target == NumericTarget::Cpu && numericScalarArithmeticAvailable(type, target);
}
constexpr std::uint64_t integerMaximum(NumericType type) {
  const auto bits = describeNumeric(type)->bits;
  return isUnsigned(type) ? (bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1)
                          : (UINT64_C(1) << (bits - 1)) - 1;
}
constexpr std::int64_t integerMinimum(NumericType type) {
  const auto bits = describeNumeric(type)->bits;
  return isUnsigned(type) ? 0 : bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
}
constexpr std::optional<std::uint64_t>
checkedNumericBytes(NumericType type, std::uint64_t count,
                    std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) {
  auto description = describeNumeric(type);
  if (!description || count > limit / description->bytes())
    return {};
  return count * description->bytes();
}
} // namespace kalandra
