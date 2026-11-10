#include "Application.h"
#include <cstdio>
extern "C" std::int32_t _mlir_ciface_kalandra_int32_entry(kalandra::runtime::Invocation *);
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (!kalandra_application_initialize_v1(argc, argv))
    return 1;
  kalandra::runtime::Invocation context;
  const auto status = _mlir_ciface_kalandra_int32_entry(&context);
  // The application result contains no owned backing. Direct native plans can
  // keep nonescaping tensor temporaries in this invocation, just as task plans
  // keep them in their child invocation. Retire them at the outer boundary.
  context.settle();
  if (context.live() || status < 0 || status > 255)
    context.fault(3);
  if (context.status() || context.hasExpectedError())
    context.reportApplicationFailure();
  return status;
}
