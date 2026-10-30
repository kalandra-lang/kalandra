#include "../runtime/Runtime.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
using kalandra::runtime::Invocation;
#ifdef FLOAT32
using Float = float;
using Bits = std::uint32_t;
#else
using Float = double;
using Bits = std::uint64_t;
#endif
extern "C" Float _mlir_ciface_kalandra_test_entry(Float, std::int64_t, std::int64_t, std::int64_t,
                                                  Invocation *);
static void check(bool value) {
  if (!value)
    throw std::runtime_error("checked floating call regression");
}
static Float reference(Float value, unsigned depth, bool rounded = false) {
  if (!depth) {
    if (!rounded)
      return value;
    const Float product = value * value;
    return product - Float(1);
  }
  const auto left = reference(value * Float(0.5), depth - 1, rounded);
  const auto right = reference(value * Float(-0.25), depth - 1, rounded);
  return left + right;
}
int main() {
  const Float witness = Float(1) + Float(3) * std::numeric_limits<Float>::epsilon();
  check(reference(witness, 0, true) != std::fma(witness, witness, Float(-1)));
  unsigned cases = 0;
  for (int mode = 0; mode < 7; ++mode)
    for (unsigned manual : {0, 1, 120, 128, 1024, 4096})
      for (int kind : {0, 1, 2})
        for (int depth : {0, 1, 2, 5, 8, 9})
          for (auto bits : {
#ifdef FLOAT32
                   0ULL, 0x80000000ULL, 1ULL, 0x00800000ULL, 0x3fa66666ULL, 0xbfa66666ULL,
                   0x7f800000ULL, 0xff800000ULL, 0x7fc00042ULL, 0x7f7fffffULL, 0xdf000000ULL,
                   0x5f000000ULL, 0x3f800003ULL
#else
                   0ULL, 0x8000000000000000ULL, 1ULL, 0x0010000000000000ULL, 0x3ff4cccccccccccdULL,
                   0xbff4cccccccccccdULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
                   0x7ff8000000000042ULL, 0x7fefffffffffffffULL, 0xc3e0000000000000ULL,
                   0x43e0000000000000ULL, 0x3ff0000000000003ULL
#endif
               })
            for (int divisor : {0, 1, 7}) {
              Invocation ctx;
              std::atomic<bool> token = mode == 4;
              std::atomic<std::uint64_t> polls = 0;
              for (unsigned i = 0; i < manual; ++i)
                check(ctx.enterCall(0, 0));
              if (mode == 1 || mode == 3 || mode == 4)
                ctx.bindCancellation(&token);
              if (mode == 2 || mode == 3)
                ctx.watchPolls(&polls);
              if (mode == 5)
                ctx.fault(5, 19, 23);
              if (mode == 6)
                ctx.settle();
              const auto input = std::bit_cast<Float>(Bits(bits));
              const auto value =
                  _mlir_ciface_kalandra_test_entry(input, depth, kind, divisor, &ctx);
              check(ctx.callDepth() == manual && ctx.live() == 0);
              if (mode == 5)
                check(ctx.status() == 5 && ctx.line() == 19 && ctx.column() == 23);
              else if (mode == 6)
                check(ctx.status() == 3);
              else if (mode == 4)
                check(value == 0 && ctx.status() == 0 && ctx.poll() == -1);
              else if (!ctx.status()) {
                check(depth <= 9);
                Float expected;
                if (kind != 1)
                  expected = reference(input, depth, kind == 2);
                else {
                  check(std::isfinite(input) && input >= -0x1p63 && input < 0x1p63 && divisor);
                  expected = Float(std::int64_t(input) / divisor);
                  if (depth && expected == 0)
                    expected = 0.0;
                }
                check((std::isnan(expected) && std::isnan(value)) ||
                      std::bit_cast<Bits>(expected) == std::bit_cast<Bits>(value));
                if ((mode == 2 || mode == 3) && depth)
                  check(polls > 1);
              } else {
                check(ctx.status() == 4);
                if (ctx.status() == 4)
                  check(kind == 1 &&
                        (!divisor || !std::isfinite(input) || input < -0x1p63 || input >= 0x1p63));
              }
              std::printf(
                  "%d %u %d %d %llu %d %016llx %lld %llu %llu %llu %zu\n", mode, manual, kind,
                  depth, bits, divisor, static_cast<unsigned long long>(std::bit_cast<Bits>(value)),
                  static_cast<long long>(ctx.status()), static_cast<unsigned long long>(ctx.line()),
                  static_cast<unsigned long long>(ctx.column()),
                  static_cast<unsigned long long>(polls.load()), ctx.callDepth());
              while (ctx.callDepth())
                ctx.leaveCall();
              ++cases;
            }
  std::fprintf(stderr, "%u independent floating/state checks passed\n", cases);
}
