#include "../runtime/Runtime.h"
#include <iostream>
using namespace kalandra::runtime;
extern "C" bool _mlir_ciface_kalandra_test_entry(bool, Invocation *);
int main() {
  for (bool flag : {false, true}) {
    Invocation context;
    const bool executes = OPERATION == 1 ? !flag : flag;
    const auto result = _mlir_ciface_kalandra_test_entry(flag, &context);
    if (context.status() != (executes ? 4 : 0) || result != (executes ? false : OPERATION == 1) ||
        (executes && (context.line() != 2 || context.column() != 3)))
      return 1;
    context.settle();
    if (context.live() || context.allocations() != context.releases())
      return 1;
  }
  std::cout << "dynamic skip op " << OPERATION
            << ": selected fault and unselected call behavior passed\n";
}
