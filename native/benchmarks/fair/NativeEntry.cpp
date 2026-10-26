// Measure the production checked entry, including fresh invocation lifecycle.
#include "../../runtime/NativeCancellation.h"
#include "../../runtime/Runtime.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using kalandra::runtime::Invocation;
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
int main(int argc, char **argv) {
  if (argc != 6) return 2;
  const int mode = std::atoi(argv[1]);
  const auto depth = std::strtoll(argv[2], nullptr, 10);
  const auto count = std::strtoll(argv[3], nullptr, 10);
  const auto seed = std::strtoll(argv[4], nullptr, 10);
  const auto expected = std::strtod(argv[5], nullptr);
  const auto tolerance = 2 * (std::nextafter(expected, std::numeric_limits<double>::infinity()) - expected);
  const bool available = kalandra::runtime::initializeNativeCancellation();
  if (mode == 3 && !available) return 3;
  double checksum = 0;
  auto batch = [&] {
    checksum = 0;
    for (std::int64_t i = 0; i != count; ++i) {
      Invocation invocation;
      std::atomic<bool> token{false};
      if (mode) invocation.bindCancellation(&token);
      invocation.allowNativeInterrupts(mode == 3);
      const auto value = _mlir_ciface_kalandra_test_entry(seed, depth, &invocation);
      if (invocation.status() || invocation.callDepth() || !std::isfinite(value) ||
          std::abs(value - expected) > tolerance)
        std::abort();
      invocation.settle();
      if (invocation.live() || invocation.allocations() != invocation.releases()) std::abort();
      checksum += value;
    }
  };
  batch();
  batch();
  const auto begin = std::chrono::steady_clock::now();
  batch();
  const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
  std::printf("{\"mode\":%d,\"depth\":%lld,\"count\":%lld,\"ns\":%.9g,\"checksum\":%.17g}\n",
              mode, depth, count, ns, checksum);
}
