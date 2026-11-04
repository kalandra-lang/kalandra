#include "../runtime/Tensor.h"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
using Array = CpuTensor<NumericType::Int64, 1>;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(Array *, std::int64_t, Invocation *);
int main() {
  try {
    for (auto position : {-1, 0, 1, 2}) {
      std::array<std::int64_t, 2> cells{7, 9}, before = cells;
      Array input{cells.data(), cells.data(), 0, {2}, {1}};
      Invocation context;
      const auto result = _mlir_ciface_kalandra_test_entry(&input, position, &context);
      const bool valid = position >= 0 && position < 2;
      if (result || context.status() != (valid ? 4 : 5) || context.line() != (valid ? 2 : 4) ||
          context.column() != 3 || std::memcmp(cells.data(), before.data(), sizeof(cells)) ||
          context.allocations())
        throw std::runtime_error(
            "destination bounds/RHS fault order changed or failed RHS wrote storage");
      context.settle();
    }
    for (bool prior : {false, true}) {
      std::array<std::int64_t, 2> cells{7, 9}, before = cells;
      Array input{cells.data(), cells.data(), 0, {2}, {1}};
      Invocation context;
      std::atomic<bool> stopped{true};
      context.bindCancellation(&stopped);
      if (prior)
        context.fault(1, 17, 4);
      const auto result = _mlir_ciface_kalandra_test_entry(&input, 0, &context);
      if (result || context.status() != (prior ? 1 : 0) ||
          std::memcmp(cells.data(), before.data(), sizeof(cells)) ||
          (prior && (context.line() != 17 || context.column() != 4)) ||
          (!prior && context.poll() != -1))
        throw std::runtime_error("cancellation replaced prior fault or wrote array storage");
      context.settle();
    }
    std::cout << "Array write faults: destination before RHS, no failing-RHS write, "
                 "cancellation/prior-fault precedence passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
