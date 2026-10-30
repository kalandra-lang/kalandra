#include "../runtime/Runtime.h"
#include <iostream>
using namespace kalandra::runtime;
extern "C" double _mlir_ciface_kalandra_test_entry(bool, Invocation *);
int main() {
  for (bool flag : {false, true}) {
    Invocation context;
    context.failAllocationAt(1);
    const auto value = _mlir_ciface_kalandra_test_entry(flag, &context);
    if (flag ? (context.status() != 0 || value != 7 || context.allocations() != 0)
             : (context.status() != 2 || value != 0))
      return 1;
    context.settle();
    if (context.live() || context.allocations() != context.releases())
      return 1;
  }
  std::cout << "conditional allocation: unselected tensor work allocates nothing; selected failure "
               "settles without a value\n";
}
