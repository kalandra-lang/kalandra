#include "String.h"
#include <cstdio>
extern "C" void _mlir_ciface_kalandra_string_entry(kalandra::runtime::StringDescriptor *,
                                                   kalandra::runtime::Invocation *);
int main(int argc, char **argv) {
  kalandra_process_arguments_v1(argc, argv);
  kalandra::runtime::Invocation context;
  kalandra::runtime::StringDescriptor value;
  _mlir_ciface_kalandra_string_entry(&value, &context);
  if (!kalandra::runtime::validateString(context, value))
    context.reportFailure();
  auto backing = value.allocated ? context.lease(value.allocated) : std::nullopt;
  context.settle();
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  if ((value.byteLength && std::fwrite(value.data, 1, static_cast<std::size_t>(value.byteLength),
                                       stdout) != static_cast<std::size_t>(value.byteLength)) ||
      std::fputc('\n', stdout) == EOF) {
    std::fputs("cannot write String result\n", stderr);
    return 1;
  }
}
