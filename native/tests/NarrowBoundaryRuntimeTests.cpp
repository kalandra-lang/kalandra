#include "../runtime/Runtime.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifndef TEST_SCALAR
#error TEST_SCALAR is required
#endif
using Scalar = TEST_SCALAR;
extern "C" Scalar _mlir_ciface_kalandra_test_entry(Scalar, kalandra::runtime::Invocation *);
int main() {
  std::uint64_t cases = 0;
  const auto low = std::int64_t(std::numeric_limits<Scalar>::min());
  const auto high = std::int64_t(std::numeric_limits<Scalar>::max());
  for (auto value = low; value <= high; ++value) {
    kalandra::runtime::Invocation context;
    // Promote at the call site: storing back into Scalar first can hide a
    // missing platform return extension by truncating the erroneous high bits.
    const std::int64_t actual = _mlir_ciface_kalandra_test_entry(Scalar(value), &context);
    if (actual != value || context.status())
      throw std::runtime_error("narrow C return extension mismatch: " + std::to_string(value) +
                               " became " + std::to_string(actual));
    context.settle();
    if (context.callDepth() || context.live() || context.allocations() != context.releases())
      throw std::runtime_error("narrow boundary did not settle");
    ++cases;
  }
  std::cout << cases << " narrow C boundary values and promoted results passed\n";
}
