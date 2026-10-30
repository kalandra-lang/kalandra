#include "../runtime/Runtime.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>
using kalandra::runtime::Invocation;
extern "C" std::int64_t (*const kalandra_callable_test_entry)(Invocation *);
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
      const auto value = kalandra_callable_test_entry(&context);
      ++probes;
      if (!attempt)
        allocations = context.allocations();
      require(allocations < 64 && value == 0, "map callback fault published a result");
      if (attempt && attempt <= allocations)
        require(context.status() == 2, "map callback lost allocation failure");
      else
        require(context.status() == 4 && context.line() == 7 && context.column() == 34,
                "map callback lost original fault location");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "faulted map callback leaked storage");
    }
    std::cout << "Map callback fault: " << probes
              << " allocation probes; source location and settled storage passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
