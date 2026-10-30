#include "../runtime/Runtime.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <type_traits>
using kalandra::runtime::Invocation;
using Scalar = TEST_SCALAR;
extern "C" Scalar _mlir_ciface_kalandra_test_entry(Scalar, std::int64_t, Invocation *);
static std::uint64_t digest = 1469598103934665603ULL, cases = 0;
static void check(bool value) {
  if (!value) throw std::runtime_error("recursive scalar conformance failed");
}
static void mix(std::uint64_t value) { digest = (digest ^ value) * 1099511628211ULL; }
template <class T> static std::uint64_t bits(T value) {
  if constexpr (std::is_same_v<T, float>) return std::bit_cast<std::uint32_t>(value);
  else if constexpr (std::is_same_v<T, double>) return std::bit_cast<std::uint64_t>(value);
  else return std::uint64_t(value);
}
template <class T> static bool twice(T &value) {
  if constexpr (std::is_same_v<T, bool>) { value = true; return true; }
  else if constexpr (std::is_integral_v<T>) {
    T next;
    if (__builtin_add_overflow(value, value, &next)) return false;
    value = next;
  } else {
    volatile T next = value + value;
    value = next;
  }
  return true;
}
static void test(Scalar input) {
  for (int mode = 0; mode != 6; ++mode)
    for (int manual : {0, 1, 4096})
      for (int n : {0, 1, 2, 3, 5, 7, 9, 10}) {
        Invocation ctx;
        for (int j = 0; j < manual; ++j) check(ctx.enterCall(0, 0));
        std::atomic<bool> token{mode == 5};
        std::atomic<std::uint64_t> polls{0};
        if (mode == 1 || mode == 3 || mode == 5) ctx.bindCancellation(&token);
        if (mode == 2 || mode == 3) ctx.watchPolls(&polls);
        if (mode == 4) ctx.fault(5, 19, 23);
        const auto result = _mlir_ciface_kalandra_test_entry(input, n, &ctx);
        check(ctx.callDepth() == std::uint64_t(manual) && ctx.live() == 0);
        if (mode == 4)
          check(result == Scalar(0) && ctx.status() == 5 && ctx.line() == 19 && ctx.column() == 23);
        else if (mode == 5)
          check(result == Scalar(0) && ctx.status() == 0 && ctx.poll() == -1);
        else {
          Scalar expected = input;
          bool valid = true;
          for (int j = 0; j < n && valid; ++j) valid = twice(expected);
          if (!valid) check(result == Scalar(0) && ctx.status() == 4);
          else {
            check(ctx.status() == 0);
            if constexpr (std::is_floating_point_v<Scalar>) {
              if (std::isnan(expected)) check(std::isnan(result));
              else check(bits(result) == bits(expected));
            } else check(result == expected);
          }
        }
        for (auto state : {bits(result), std::uint64_t(ctx.status()), std::uint64_t(ctx.line()),
                            std::uint64_t(ctx.column()), ctx.callDepth(), polls.load()}) mix(state);
        for (int j = 0; j < manual; ++j) ctx.leaveCall();
        check(ctx.callDepth() == 0);
        ++cases;
      }
}
int main() {
  test(Scalar(0)); test(Scalar(1)); test(std::numeric_limits<Scalar>::max());
  test(std::numeric_limits<Scalar>::lowest());
  if constexpr (std::is_floating_point_v<Scalar>) {
    test(Scalar(-0.0)); test(Scalar(1.25)); test(Scalar(-1.25));
    test(std::numeric_limits<Scalar>::denorm_min());
    test(std::numeric_limits<Scalar>::infinity());
    test(-std::numeric_limits<Scalar>::infinity());
    test(std::numeric_limits<Scalar>::quiet_NaN());
  } else if constexpr (!std::is_same_v<Scalar, bool>) {
    test(Scalar(7)); test(Scalar(37));
    if constexpr (std::is_signed_v<Scalar>) test(Scalar(-7));
  }
  std::printf("%llu recursive scalar/state cases; digest %llu\n",
              static_cast<unsigned long long>(cases), static_cast<unsigned long long>(digest));
}
