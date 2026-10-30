#include "../runtime/Runtime.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
extern "C" std::int64_t _mlir_ciface_kalandra_test_entry(std::int64_t, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::uint64_t allocations = 0;
    unsigned probes = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation context;
      context.failAllocationAt(attempt);
      const auto value = _mlir_ciface_kalandra_test_entry(0, &context);
      ++probes;
      if (!attempt)
        allocations = context.allocations();
      require(allocations < 64 && value == 0, "block closure fault published a result");
      if (attempt && attempt <= allocations)
        require(context.status() == 2, "block closure lost allocation failure");
      else
        require(context.status() == 4 && context.line() == 4 && context.column() == 5,
                "block body lost original fault location");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "faulted block closure leaked storage");
    }
    std::cout << "Block closure fault: " << probes
              << " allocation probes; source location and settled storage passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
