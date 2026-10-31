#include "Runtime.h"
#include <cinttypes>
#include <cstdio>
extern "C" std::uint64_t _mlir_ciface_kalandra_uint64_entry(kalandra::runtime::Invocation *);
int main(int argc, char **argv) {
  kalandra_process_arguments_v1(argc, argv);
  kalandra::runtime::Invocation context;
  const auto value = _mlir_ciface_kalandra_uint64_entry(&context);
  if (context.live() != 0)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  std::printf("%" PRIu64 "\n", value);
}
