#include "../runtime/Runtime.h"
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
using kalandra::runtime::Invocation;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
static std::uint64_t digest = 1469598103934665603ULL;
static std::uint64_t cases = 0;
static void check(bool value) {
  if (!value)
    throw std::runtime_error("signed remainder conformance failed");
}
static void mix(std::uint64_t value) {
  digest = (digest ^ value) * 1099511628211ULL;
}
static void test(std::int64_t a, std::int64_t b) {
  for (int mode = 0; mode != 6; ++mode) {
    Invocation ctx;
    const int manual = int(cases % 3) * 60;
    for (int n = 0; n < manual; ++n)
      check(ctx.enterCall(0, 0));
    std::atomic<bool> token{mode == 5};
    std::atomic<std::uint64_t> polls{0};
    if (mode == 1 || mode == 3 || mode == 5)
      ctx.bindCancellation(&token);
    if (mode == 2 || mode == 3)
      ctx.watchPolls(&polls);
    if (mode == 4)
      ctx.fault(5, 19, 23);
    const auto value = _mlir_ciface_kalandra_test_entry(a, b, &ctx);
    check(ctx.callDepth() == std::uint64_t(manual) && ctx.live() == 0);
    if (mode == 4)
      check(value == 0 && ctx.status() == 5 && ctx.line() == 19 && ctx.column() == 23);
    else if (mode == 5)
      check(value == 0 && ctx.status() == 0 && ctx.poll() == -1);
    else if (!b)
      check(value == 0 && ctx.status() == 4 && ctx.line() == 2 && ctx.column() == 3);
    else {
      // C++'s only overflowing pair is handled explicitly in the reference.
      const auto expected = a == std::numeric_limits<std::int64_t>::min() && b == -1 ? 0 : a % b;
      check(ctx.status() == 0 && value == expected);
    }
    for (auto state : {std::uint64_t(value), std::uint64_t(ctx.status()),
                       std::uint64_t(ctx.line()), std::uint64_t(ctx.column()),
                       std::uint64_t(ctx.callDepth()), polls.load()})
      mix(state);
    for (int n = 0; n < manual; ++n)
      ctx.leaveCall();
    check(ctx.callDepth() == 0);
    ++cases;
  }
}
int main() {
  // Exhaust the signed 8-bit domain, including zero divisors, using int64 IR.
  for (int a = -128; a < 128; ++a)
    for (int b = -128; b < 128; ++b)
      test(a, b);
  const std::int64_t edges[] = {INT64_MIN, INT64_MIN + 1, -65536, -32768, -129, -128,
      -127, -2, -1, 0, 1, 2, 127, 128, 129, 32767, 65535, INT64_MAX - 1, INT64_MAX};
  for (auto a : edges)
    for (auto b : edges)
      test(a, b);
  std::uint64_t random = 0x9e3779b97f4a7c15ULL;
  auto next = [&] {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return std::bit_cast<std::int64_t>(random);
  };
  for (unsigned i = 0; i != 16384; ++i) {
    const auto a = next();
    const auto b = next();
    test(a, b);
    test(a, -1);
  }
  std::printf("%llu signed remainder/state cases; digest %llu\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(digest));
}
