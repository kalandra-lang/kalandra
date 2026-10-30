#pragma once
#include "kalandra/NumericType.h"
#include "llvm/ADT/APInt.h"
#include <array>
#include <cstdint>
#include <vector>

namespace integer_proof {
using namespace kalandra;
using Bits = std::uint64_t;
struct Pair {
  Bits a, b;
};
struct Expected {
  Bits bits;
  bool valid;
};
using Result = Bits (*)(Bits, Bits);
using Validity = std::uint32_t (*)(Bits, Bits);
#define DECLARE_OP(TYPE, OP)                                                                       \
  extern "C" Bits checked_##TYPE##_##OP(Bits, Bits);                                               \
  extern "C" std::uint32_t valid_##TYPE##_##OP(Bits, Bits);
#define DECLARE(TYPE)                                                                              \
  DECLARE_OP(TYPE, add)                                                                            \
  DECLARE_OP(TYPE, sub) DECLARE_OP(TYPE, mul) DECLARE_OP(TYPE, div) DECLARE_OP(TYPE, rem)          \
      DECLARE_OP(TYPE, neg)
DECLARE(int8)
DECLARE(uint8) DECLARE(int16) DECLARE(uint16) DECLARE(int32) DECLARE(uint32) DECLARE(int64)
    DECLARE(uint64)
#undef DECLARE
#undef DECLARE_OP
        struct TypeCase {
  NumericType type;
  const char *name;
  std::array<Result, 6> results;
  std::array<Validity, 6> valid;
};
#define CASE(TYPE, ID)                                                                             \
  TypeCase {                                                                                       \
    NumericType::ID, #TYPE, {checked_##TYPE##_add, checked_##TYPE##_sub, checked_##TYPE##_mul,     \
                             checked_##TYPE##_div, checked_##TYPE##_rem, checked_##TYPE##_neg},    \
    {                                                                                              \
      valid_##TYPE##_add, valid_##TYPE##_sub, valid_##TYPE##_mul, valid_##TYPE##_div,              \
          valid_##TYPE##_rem, valid_##TYPE##_neg                                                   \
    }                                                                                              \
  }
inline constexpr std::array cases{CASE(int8, Int8),     CASE(uint8, UInt8),  CASE(int16, Int16),
                                  CASE(uint16, UInt16), CASE(int32, Int32),  CASE(uint32, UInt32),
                                  CASE(int64, Int64),   CASE(uint64, UInt64)};
#undef CASE
inline Expected oracle(NumericType type, Pair pair, unsigned op) {
  const auto width = describeNumeric(type)->bits;
  const Bits mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
  llvm::APInt a(width, pair.a & mask), b(width, pair.b & mask), result(width, 0);
  bool overflow = false;
  const bool u = isUnsigned(type);
  switch (op) {
  case 0:
    result = u ? a.uadd_ov(b, overflow) : a.sadd_ov(b, overflow);
    break;
  case 1:
    result = u ? a.usub_ov(b, overflow) : a.ssub_ov(b, overflow);
    break;
  case 2:
    result = u ? a.umul_ov(b, overflow) : a.smul_ov(b, overflow);
    break;
  case 3:
  case 4:
    if (b.isZero())
      overflow = true;
    else if (!u && a.isMinSignedValue() && b.isAllOnes()) {
      overflow = op == 3;
    } else
      result = op == 3 ? (u ? a.udiv(b) : a.sdiv(b)) : (u ? a.urem(b) : a.srem(b));
    break;
  case 5:
    overflow = u ? !a.isZero() : a.isMinSignedValue();
    result = -a;
    break;
  }
  return {result.getZExtValue(), !overflow};
}
inline std::vector<Pair> corpus(NumericType type) {
  const auto width = describeNumeric(type)->bits;
  const Bits mask = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
  const Bits sign = UINT64_C(1) << (width - 1);
  std::vector<Pair> result;
  if (width == 8) {
    for (Bits a = 0; a <= mask; ++a)
      for (Bits b = 0; b <= mask; ++b)
        result.push_back({a, b});
  } else {
    std::vector<Bits> edges{0, 1, 2, 3, mask, mask - 1, sign, sign - 1, sign + 1};
    for (unsigned bit = 0; bit < width; ++bit) {
      edges.push_back(Bits{1} << bit);
      edges.push_back((Bits{1} << bit) - 1);
      edges.push_back(mask ^ (Bits{1} << bit));
    }
    for (auto a : edges)
      for (auto b : edges)
        result.push_back({a, b});
    Bits state = UINT64_C(0x219834adffcc5817);
    auto random = [&] {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    };
    for (unsigned i = 0; i < 16384; ++i)
      result.push_back({random(), random()});
  }
  return result;
}
} // namespace integer_proof
