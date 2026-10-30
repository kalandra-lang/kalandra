#include "../runtime/Runtime.h"
#include <iostream>
using namespace kalandra::runtime;
extern "C" bool _mlir_ciface_kalandra_test_entry(bool, bool, Invocation *);
int main() {
  for (bool a : {false, true})
    for (bool b : {false, true}) {
      Invocation context;
      const auto expected = OPERATION == 0 ? a == b : a != b;
      auto actual = _mlir_ciface_kalandra_test_entry(a, b, &context);
      context.settle();
      if (actual != expected || context.status() || context.live() ||
          context.allocations() != context.releases())
        return 1;
    }
  std::cout << "Bool equality op " << OPERATION
            << ": all four native parameter/result pairs passed\n";
}
