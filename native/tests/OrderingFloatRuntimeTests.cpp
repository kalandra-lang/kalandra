#include "../runtime/Runtime.h"
#include <array>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
using kalandra::runtime::Invocation;
#ifdef FLOAT32
using Float = float;
using Bits = std::uint32_t;
#else
using Float = double;
using Bits = std::uint64_t;
#endif
#ifdef CALLABLE_PLAN
extern "C" std::int64_t (*const kalandra_callable_test_entry)(Float, Float, Invocation *);
#define _mlir_ciface_kalandra_test_entry kalandra_callable_test_entry
#else
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Float, Float, Invocation *);
#endif
static void require(bool value) {
  if (!value)
    throw std::runtime_error("float representation ordering differs from independent oracle");
}
static int reference(Bits a, Bits b) {
  if (a == b)
    return 0;
  constexpr Bits sign = Bits(1) << (sizeof(Bits) * 8 - 1);
  const bool negative = (a & sign) != 0;
  if (negative != ((b & sign) != 0))
    return negative ? -1 : 1;
  return negative ? (a > b ? -1 : 1) : (a < b ? -1 : 1);
}
static int qualify() {
  constexpr Bits sign = Bits(1) << (sizeof(Bits) * 8 - 1);
  constexpr Bits fraction = sizeof(Bits) == 4 ? Bits(0x007fffffULL) : Bits(0x000fffffffffffffULL);
  constexpr Bits infinity = sizeof(Bits) == 4 ? Bits(0x7f800000ULL) : Bits(0x7ff0000000000000ULL);
  constexpr Bits quiet = (fraction >> 1) + 1;
  std::vector<Bits> edges;
  for (Bits a : {Bits(0), Bits(1), fraction, Bits(fraction + 1), Bits(infinity - 1), infinity,
                 Bits(infinity | 1), Bits(infinity | (quiet - 1)), Bits(infinity | quiet),
                 Bits(infinity | quiet | 42), Bits(infinity | fraction)})
    for (Bits s : {Bits(0), sign})
      edges.push_back(a | s);
  std::uint64_t cases = 0, state = 0x93a6858c9e11b63bULL;
  const auto check = [&](Bits a, Bits b) {
    Invocation context;
    std::feclearexcept(FE_ALL_EXCEPT);
    const auto result = _mlir_ciface_kalandra_test_entry(std::bit_cast<Float>(a),
                                                         std::bit_cast<Float>(b), &context);
    const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
    if (flags || context.status() || result != reference(a, b)) {
      std::fprintf(stderr, "a=%llx b=%llx result=%lld expected=%d status=%llu flags=%d round=%d\n",
                   static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                   static_cast<long long>(result), reference(a, b),
                   static_cast<unsigned long long>(context.status()), flags, std::fegetround());
      require(false);
    }
    context.settle();
    require(!context.live() && context.allocations() == context.releases());
    ++cases;
  };
  for (auto mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    require(std::fesetround(mode) == 0);
    for (auto a : edges)
      for (auto b : edges)
        check(a, b);
    for (unsigned i = 0; i != 8192; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const auto a = Bits(state);
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      check(a, Bits(state));
    }
  }
  require(std::fesetround(FE_TONEAREST) == 0);
  Invocation canceled;
  std::atomic<bool> token = true;
  canceled.bindCancellation(&token);
  const auto canceledResult = _mlir_ciface_kalandra_test_entry(Float(0), Float(1), &canceled);
  if (canceledResult != 0 || canceled.status() != 0 || canceled.poll() != -1) {
    std::fprintf(stderr, "cancellation result=%lld status=%llu\n",
                 static_cast<long long>(canceledResult),
                 static_cast<unsigned long long>(canceled.status()));
    require(false);
  }
  canceled.settle();
  require(!canceled.live());
  std::printf("%llu exact float-order pairs; four rounding modes, signaling/quiet NaNs, signed "
              "zero, cleanup and cancellation passed\n",
              static_cast<unsigned long long>(cases));
  return 0;
}
int main() {
  try {
    return qualify();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
