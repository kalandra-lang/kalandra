#include "OrdinaryFault.h"
#include "Runtime.h"
#include <cstdio>
#include <cstdlib>

// Private executable adapter; constructing an Invocation is confined to failure.
extern "C" std::int32_t
kalandra_ordinary_finish_v1(std::uint64_t bits, std::int32_t kind,
                            const kalandra::runtime::OrdinaryFault *fault) noexcept {
  using namespace kalandra::runtime;
  const auto check = [&](auto value) {
    if (ordinaryFaulted(value, *fault)) {
      Invocation context;
      context.fault(fault->code, fault->line, fault->column);
      context.reportFailure();
    }
    return value;
  };
  switch (kind) {
  case 0:
    std::printf("%.9g\n", static_cast<double>(check(std::bit_cast<float>(std::uint32_t(bits)))));
    break;
  case 1:
    std::printf("%.17g\n", check(std::bit_cast<double>(bits)));
    break;
  case 2:
    std::printf("%s\n", check(bool(bits)) ? "true" : "false");
    break;
  case 3:
    std::printf("%d\n", check(std::bit_cast<std::int32_t>(std::uint32_t(bits))));
    break;
  case 4:
    std::printf("%u\n", check(std::uint32_t(bits)));
    break;
  case 5:
    std::printf("%lld\n", static_cast<long long>(check(std::bit_cast<std::int64_t>(bits))));
    break;
  case 6:
    std::printf("%llu\n", static_cast<unsigned long long>(check(bits)));
    break;
  default:
    std::abort();
  }
  return 0;
}

extern "C" void kalandra_ordinary_begin_v1(void *storage) noexcept {
  new (storage) kalandra::runtime::Invocation;
}
extern "C" void kalandra_ordinary_end_v1(kalandra::runtime::Invocation *context,
                                         kalandra::runtime::OrdinaryFault *fault) noexcept {
  *fault = {context->status(), context->line(), context->column()};
  context->~Invocation();
}

// Same process-failure boundary as ordinary_finish_v1, reached only on error.
// Eligibility excludes live caller resources and all task/cancellable entries.
extern "C" [[noreturn]] void kalandra_ordinary_fail_v1(std::uint64_t code, std::uint64_t line,
                                                       std::uint64_t column) noexcept {
  kalandra::runtime::Invocation context;
  context.fault(code ? code : 3, line, column);
  context.reportFailure();
}
