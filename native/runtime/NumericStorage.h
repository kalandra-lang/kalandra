#pragma once
#include "../include/kalandra/NumericType.h"
#include "Tasks.h"
#include <cstring>
#include <type_traits>
namespace kalandra::runtime {
template <NumericType> struct NumericStorage;
#define KALANDRA_NUMERIC_STORAGE(Name, Type)                                                       \
  template <> struct NumericStorage<NumericType::Name> {                                           \
    using Value = Type;                                                                            \
  };                                                                                               \
  static_assert(sizeof(Type) == describeNumeric(NumericType::Name)->bytes())
KALANDRA_NUMERIC_STORAGE(Int8, std::int8_t);
KALANDRA_NUMERIC_STORAGE(Int16, std::int16_t);
KALANDRA_NUMERIC_STORAGE(Int32, std::int32_t);
KALANDRA_NUMERIC_STORAGE(Int64, std::int64_t);
KALANDRA_NUMERIC_STORAGE(UInt8, std::uint8_t);
KALANDRA_NUMERIC_STORAGE(UInt16, std::uint16_t);
KALANDRA_NUMERIC_STORAGE(UInt32, std::uint32_t);
KALANDRA_NUMERIC_STORAGE(UInt64, std::uint64_t);
KALANDRA_NUMERIC_STORAGE(Float32, float);
KALANDRA_NUMERIC_STORAGE(Float64, double);
#undef KALANDRA_NUMERIC_STORAGE
template <NumericType Type> using NumericValue = typename NumericStorage<Type>::Value;
constexpr std::optional<std::size_t> numericAlignment(NumericType type) {
  switch (type) {
#define KALANDRA_ALIGNMENT(Name)                                                                   \
  case NumericType::Name:                                                                          \
    return alignof(NumericValue<NumericType::Name>)
    KALANDRA_ALIGNMENT(Int8);
    KALANDRA_ALIGNMENT(Int16);
    KALANDRA_ALIGNMENT(Int32);
    KALANDRA_ALIGNMENT(Int64);
    KALANDRA_ALIGNMENT(UInt8);
    KALANDRA_ALIGNMENT(UInt16);
    KALANDRA_ALIGNMENT(UInt32);
    KALANDRA_ALIGNMENT(UInt64);
    KALANDRA_ALIGNMENT(Float32);
    KALANDRA_ALIGNMENT(Float64);
#undef KALANDRA_ALIGNMENT
  }
  return {};
}
template <NumericType Type>
bool copyNumericScalar(Invocation &, void *output, const Invocation &, const void *input) noexcept {
  std::memcpy(output, input, sizeof(NumericValue<Type>));
  return true;
}
inline void destroyNumericScalar(void *) noexcept {}
// Private process-local layouts, not serialized numeric type IDs. Keep the
// existing float32 bridge identity while distinguishing every other family/width.
template <NumericType Type> constexpr ValueLayout numericScalarLayoutValue() {
  return {Type == NumericType::Float32 ? 0x4b414c46533332ULL
                                       : 0x4b414c4e00000000ULL + static_cast<unsigned>(Type) + 1,
          sizeof(NumericValue<Type>), alignof(NumericValue<Type>), copyNumericScalar<Type>,
          destroyNumericScalar};
}
inline constexpr std::array numericScalarLayouts{numericScalarLayoutValue<NumericType::Int8>(),
                                                 numericScalarLayoutValue<NumericType::Int16>(),
                                                 numericScalarLayoutValue<NumericType::Int32>(),
                                                 numericScalarLayoutValue<NumericType::Int64>(),
                                                 numericScalarLayoutValue<NumericType::UInt8>(),
                                                 numericScalarLayoutValue<NumericType::UInt16>(),
                                                 numericScalarLayoutValue<NumericType::UInt32>(),
                                                 numericScalarLayoutValue<NumericType::UInt64>(),
                                                 numericScalarLayoutValue<NumericType::Float32>(),
                                                 numericScalarLayoutValue<NumericType::Float64>()};
constexpr const ValueLayout *numericScalarLayout(NumericType type) noexcept {
  for (std::size_t i = 0; i < numericTypes.size(); ++i)
    if (numericTypes[i].type == type)
      return &numericScalarLayouts[i];
  return nullptr;
}
} // namespace kalandra::runtime
