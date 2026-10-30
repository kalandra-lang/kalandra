#include "../runtime/Runtime.h"
#include <iostream>
using namespace kalandra::runtime;
extern "C" bool _mlir_ciface_kalandra_test_entry(bool, bool, Invocation *);
int main() {
  for (bool a : {false, true})
    for (bool b : {false, true}) {
      Invocation context;
      const auto expected = OPERATION == 0   ? a && b
                            : OPERATION == 1 ? a || b
                            : OPERATION == 2 ? !a
                            : OPERATION == 3 ? a != b
                                             : (!a && b) || a;
      const auto result = _mlir_ciface_kalandra_test_entry(a, b, &context);
      context.settle();
      if (result != expected || context.status() || context.live() ||
          context.allocations() != context.releases())
        return 1;
    }
  std::cout << "logical op " << OPERATION << ": all four dynamic Bool input pairs passed\n";
}
