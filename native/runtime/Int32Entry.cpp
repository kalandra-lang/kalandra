#include "Runtime.h"
#include <cinttypes>
#include <cstdio>
extern "C" std::int32_t _mlir_ciface_kalandra_int32_entry(kalandra::runtime::Invocation *);
int main() {
  kalandra::runtime::Invocation context;
  const auto value = _mlir_ciface_kalandra_int32_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  std::printf("%" PRId32 "\n", value);
}
