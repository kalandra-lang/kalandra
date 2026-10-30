#include "Runtime.h"
#include <cstdio>
extern "C" bool _mlir_ciface_kalandra_bool_entry(kalandra::runtime::Invocation *);
int main() {
  kalandra::runtime::Invocation context;
  const auto value = _mlir_ciface_kalandra_bool_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  std::puts(value ? "true" : "false");
}
