#pragma once
#include "kalandra/NumericType.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"
#include <algorithm>
#include <cstdint>
#include <vector>
namespace conversion_proof {
using namespace kalandra;
struct Expected {
  std::uint64_t bits;
  bool valid;
};
inline const llvm::fltSemantics &semantics(NumericType type) {
  return type == NumericType::Float32 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
}
inline Expected oracle(NumericType source, NumericType destination, std::uint64_t bits) {
  const auto src = describeNumeric(source), dst = describeNumeric(destination);
  llvm::APInt input(src->bits, bits);
  if (source == destination)
    return {input.getZExtValue(), true};
  if (isInteger(source) && isInteger(destination)) {
    auto wide = isUnsigned(source) ? input.zext(128) : input.sext(128);
    auto minimum = isUnsigned(destination) ? llvm::APInt(128, 0)
                                           : llvm::APInt::getSignedMinValue(dst->bits).sext(128);
    auto maximum = isUnsigned(destination) ? llvm::APInt::getMaxValue(dst->bits).zext(128)
                                           : llvm::APInt::getSignedMaxValue(dst->bits).sext(128);
    return {wide.trunc(dst->bits).getZExtValue(), wide.sge(minimum) && wide.sle(maximum)};
  }
  if (isInteger(destination)) {
    llvm::APFloat value(semantics(source), input);
    llvm::APSInt result(dst->bits, isUnsigned(destination));
    bool exact;
    auto status = value.convertToInteger(result, llvm::APFloat::rmTowardZero, &exact);
    return {result.getZExtValue(), !(status & llvm::APFloat::opInvalidOp)};
  }
  llvm::APFloat value(semantics(destination));
  llvm::APFloat::opStatus status;
  bool finite = true;
  if (isInteger(source))
    status = value.convertFromAPInt(input, !isUnsigned(source), llvm::APFloat::rmNearestTiesToEven);
  else {
    value = llvm::APFloat(semantics(source), input);
    finite = value.isFinite();
    bool loses;
    status = value.convert(semantics(destination), llvm::APFloat::rmNearestTiesToEven, &loses);
  }
  const auto result = value.isNaN()
                          ? (dst->bits == 32 ? UINT64_C(0x7fc00000) : UINT64_C(0x7ff8000000000000))
                          : value.bitcastToAPInt().getZExtValue();
  return {result, !finite || !(status & llvm::APFloat::opOverflow)};
}
inline std::vector<std::uint64_t> corpus(NumericType source) {
  const auto bits = describeNumeric(source)->bits;
  const auto mask = bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
  std::vector<std::uint64_t> values;
  if (isInteger(source) && bits <= 16) {
    for (std::uint64_t i = 0; i <= mask; ++i)
      values.push_back(i);
  } else {
    values = {0, 1, mask, mask - 1};
    for (unsigned bit = 0; bit < bits; ++bit) {
      const auto v = UINT64_C(1) << bit;
      for (auto n : {v - 1, v, v + 1, (~v) & mask})
        values.push_back(n & mask);
    }
    if (!isInteger(source)) {
      const unsigned fraction = bits == 32 ? 23 : 52, fields = bits == 32 ? 256 : 2048;
      const auto fractionMask = (UINT64_C(1) << fraction) - 1;
      for (unsigned sign = 0; sign < 2; ++sign)
        for (unsigned exponent = 0; exponent < fields; ++exponent)
          for (auto payload :
               {UINT64_C(0), UINT64_C(1), UINT64_C(1) << (fraction - 1), fractionMask})
            values.push_back((std::uint64_t(sign) << (bits - 1)) |
                             (std::uint64_t(exponent) << fraction) | payload);
    }
    std::uint64_t state = UINT64_C(0x42d986aff3107521);
    for (unsigned i = 0; i < 8192; ++i) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      values.push_back(state & mask);
    }
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}
#define CONVERSION_DECLARE(T)                                                                      \
  extern "C" std::uint64_t converted_##T(std::uint64_t, std::uint32_t);                            \
  extern "C" std::uint32_t conversion_valid_##T(std::uint64_t, std::uint32_t);
CONVERSION_DECLARE(int8)
CONVERSION_DECLARE(int16) CONVERSION_DECLARE(int32) CONVERSION_DECLARE(int64)
    CONVERSION_DECLARE(uint8) CONVERSION_DECLARE(uint16) CONVERSION_DECLARE(uint32)
        CONVERSION_DECLARE(uint64) CONVERSION_DECLARE(float32) CONVERSION_DECLARE(float64)
#undef CONVERSION_DECLARE
            struct Case {
  NumericType type;
  const char *name;
  std::uint64_t (*bits)(std::uint64_t, std::uint32_t);
  std::uint32_t (*valid)(std::uint64_t, std::uint32_t);
};
#define CONVERSION_CASE(T, E)                                                                      \
  Case { NumericType::E, #T, converted_##T, conversion_valid_##T }
inline const Case cases[] = {CONVERSION_CASE(int8, Int8),       CONVERSION_CASE(int16, Int16),
                             CONVERSION_CASE(int32, Int32),     CONVERSION_CASE(int64, Int64),
                             CONVERSION_CASE(uint8, UInt8),     CONVERSION_CASE(uint16, UInt16),
                             CONVERSION_CASE(uint32, UInt32),   CONVERSION_CASE(uint64, UInt64),
                             CONVERSION_CASE(float32, Float32), CONVERSION_CASE(float64, Float64)};
#undef CONVERSION_CASE
} // namespace conversion_proof
