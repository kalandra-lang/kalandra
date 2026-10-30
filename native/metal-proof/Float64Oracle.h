#pragma once
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace float64_proof {

using Bits = std::uint64_t;
constexpr Bits Sign = Bits{1} << 63, Fraction = (Bits{1} << 52) - 1, Infinity = 0x7ff0000000000000,
               Nan = Infinity | (Bits{1} << 51);
struct Pair {
  Bits a, b;
};
using Results = std::array<Bits, 6>;
inline Bits oracle(Pair pair, unsigned op) {
  llvm::APFloat value(llvm::APFloat::IEEEdouble(), llvm::APInt(64, pair.a));
  llvm::APFloat right(llvm::APFloat::IEEEdouble(), llvm::APInt(64, pair.b));
  constexpr auto rounding = llvm::APFloat::rmNearestTiesToEven;
  switch (op) {
  case 0:
    value.add(right, rounding);
    break;
  case 1:
    value.multiply(right, rounding);
    break;
  case 2: {
    auto left = value;
    value.multiply(right, rounding);
    value.add(left, rounding);
    break;
  }
  case 3:
    value.subtract(right, rounding);
    break;
  case 4:
    value.changeSign();
    break;
  case 5:
    value.divide(right, rounding);
    break;
  }
  // Arithmetic canonicalizes NaNs. Unary negation preserves every payload/sign bit.
  return value.isNaN() && op != 4 ? Nan : value.bitcastToAPInt().getZExtValue();
}
inline std::vector<Pair> corpus() {
  std::vector<Pair> pairs;
  std::vector<Bits> edges;
  constexpr unsigned Exponents[] = {0,    1,    2,    510,  511,  512,  1021, 1022, 1023,
                                    1024, 1025, 1534, 1535, 1536, 2045, 2046, 2047};
  for (Bits sign : {Bits{0}, Sign})
    for (auto exp : Exponents)
      for (Bits fraction :
           {Bits{0}, Bits{1}, Fraction / 2, Fraction / 2 + 1, Fraction - 1, Fraction})
        edges.push_back(sign | (Bits(exp) << 52) | fraction);
  for (auto a : edges)
    for (auto b : edges)
      pairs.push_back({a, b});
  // Every encoded exponent, in both operands, with both signs and boundary significands.
  for (Bits exp = 0; exp < 2048; ++exp)
    for (auto edge : Exponents)
      for (unsigned signs = 0; signs < 4; ++signs) {
        Bits a = ((signs & 1) ? Sign : 0) | (exp << 52) | ((exp & 1) ? Fraction : 1);
        Bits b = ((signs & 2) ? Sign : 0) | (Bits(edge) << 52) |
                 ((exp & 1) ? Fraction / 2 + 1 : Fraction / 2);
        pairs.push_back({a, b});
        pairs.push_back({b, a});
      }
  // Every subnormal leading bit, cancellation distance and exponent-floor boundary.
  for (unsigned bit = 0; bit < 52; ++bit)
    for (Bits sign : {Bits{0}, Sign}) {
      for (Bits scale : {Bits{0x0010000000000000}, Bits{0x3fe0000000000000},
                         Bits{0x3ff0000000000000}, Bits{0x4000000000000000}})
        pairs.push_back({sign | (Bits{1} << bit), scale});
      for (Bits exp : {Bits{1}, Bits{2}, Bits{1023}, Bits{2046}})
        pairs.push_back({sign | (exp << 52) | (Bits{1} << bit), (sign ^ Sign) | (exp << 52)});
    }
  Bits seed = 0xd1934a7b5c8e20f1;
  auto next = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
  };
  for (unsigned i = 0; i < 131072; ++i)
    pairs.push_back({next(), next()});
  for (unsigned i = 0; i < 4096; ++i) {
    const auto a = next() & ~Sign;
    for (Bits delta : {Bits{0}, Bits{1}, Bits{2}})
      pairs.push_back({a, (a + delta) ^ Sign});
  }
  // Halfway ties, overflow/underflow neighbors, signed zero and FMA-sensitive cancellation.
  for (Bits sign : {Bits{0}, Sign})
    for (Bits i = 0; i < 128; ++i)
      for (Bits denominator : {Bits{0x3fe0000000000000}, Bits{0x3fe0000000000001},
                               Bits{0x3ff0000000000001}, Bits{0x4008000000000000}})
        for (Bits numerator : {i + 1, Bits{0x0010000000000000} - i, Bits{0x7fefffffffffffff} - i})
          pairs.push_back({sign | numerator, denominator});
  pairs.push_back({0x3ff0000000000001, 0xbfeffffffffffffe});
  pairs.push_back({0x3ff0000000000000, 0x3ca0000000000000});
  pairs.push_back({0x3ff0000000000001, 0x3ca0000000000000});
  return pairs;
}
// Save/restore the complete host state even on exceptions. Integer-lowered arithmetic
// must ignore rounding, flush-to-zero and default-NaN controls and raise no FP flags.
struct FpState {
  Bits control, status;
  FpState() {
    asm volatile("mrs %0, fpcr" : "=r"(control));
    asm volatile("mrs %0, fpsr" : "=r"(status));
  }
  ~FpState() {
    asm volatile("msr fpcr, %0\n\tisb" ::"r"(control) : "memory");
    asm volatile("msr fpsr, %0" ::"r"(status) : "memory");
  }
  void set(unsigned mode) const {
    constexpr Bits Mask = (Bits{3} << 22) | (Bits{1} << 24) | (Bits{1} << 25) | (Bits{31} << 8);
    const Bits value = (control & ~Mask) | (Bits(mode & 3) << 22) |
                       ((mode & 4) ? (Bits{1} << 24) | (Bits{1} << 25) : 0);
    asm volatile("msr fpcr, %0\n\tisb" ::"r"(value) : "memory");
    Bits zero = 0;
    asm volatile("msr fpsr, %0" ::"r"(zero) : "memory");
    Bits read;
    asm volatile("mrs %0, fpcr" : "=r"(read));
    if (read != value)
      throw std::runtime_error("requested FP environment was not installed");
  }
  void checkFlags() const {
    Bits flags;
    asm volatile("mrs %0, fpsr" : "=r"(flags));
    if (flags & 0x9f)
      throw std::runtime_error("integer-lowered arithmetic raised floating exception flags");
  }
};
} // namespace float64_proof
