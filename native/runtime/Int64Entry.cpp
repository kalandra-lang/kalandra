#include "Runtime.h"
#include <cinttypes>
#include <cstdio>
extern "C" std::int64_t _mlir_ciface_kalandra_int64_entry(kalandra::runtime::Invocation *);
int main() {
  kalandra::runtime::Invocation context;
  const auto value = _mlir_ciface_kalandra_int64_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  std::printf("%" PRId64 "\n", value);
}
