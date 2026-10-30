#include "Runtime.h"
#include <cstdio>
extern "C" double _mlir_ciface_kalandra_float64_entry(kalandra::runtime::Invocation *);
int main() {
  kalandra::runtime::Invocation context;
  const auto value = _mlir_ciface_kalandra_float64_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  std::printf("%.17g\n", static_cast<double>(value));
}
