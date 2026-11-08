#include "Runtime.h"
extern "C" bool _mlir_ciface_kalandra_unit_entry(kalandra::runtime::Invocation *);
int main() {
  kalandra::runtime::Invocation context;
  _mlir_ciface_kalandra_unit_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
}
