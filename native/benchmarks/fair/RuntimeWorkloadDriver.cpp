#include "../../runtime/NativeCancellation.h"
#include "../../runtime/Runtime.h"
#include "../../runtime/OrdinaryFault.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using kalandra::runtime::Invocation;
#if defined(FIXED_ENTRY)
extern "C" double ENTRY_NAME(Invocation *);
#elif defined(KALANDRA_ORDINARY_DIRECT)
extern "C" double _mlir_ciface_kalandra_test_entry_ordinary_v1(std::int64_t,std::int64_t,
                                                             kalandra::runtime::OrdinaryFault *);
#elif defined(KALANDRA_KERNEL)
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t, Invocation *);
#else
extern "C" double runtime_workload(std::int64_t, std::int64_t, std::int64_t);
#endif
int main(int argc, char **argv) {
  if (argc != 7) return 2;
  const auto kind = std::strtoll(argv[1], nullptr, 10), seed = std::strtoll(argv[2], nullptr, 10),
             rounds = std::strtoll(argv[3], nullptr, 10), count = std::strtoll(argv[4], nullptr, 10),
             batches = std::strtoll(argv[5], nullptr, 10);
  const auto expected = std::strtod(argv[6], nullptr);
  if (kind < 0 || kind > 1 || seed < 1 || seed > 31 || rounds < 0 || rounds > 4096 ||
      count < 1 || batches < 1 || !std::isfinite(expected)) return 2;
#if (defined(KALANDRA_KERNEL) || defined(FIXED_ENTRY)) && !defined(KALANDRA_ORDINARY_EXECUTION)
  if (!kalandra::runtime::initializeNativeCancellation()) return 3;
#endif
  for (long long batch = -2; batch < batches; ++batch) {
    double checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < count; ++i) {
#if defined(KALANDRA_ORDINARY_DIRECT)
      kalandra::runtime::OrdinaryFault fault;
      const auto value = _mlir_ciface_kalandra_test_entry_ordinary_v1(seed,rounds,&fault);
      if (kalandra::runtime::ordinaryFaulted(value,fault)) std::abort();
#elif defined(KALANDRA_KERNEL) || defined(FIXED_ENTRY)
      Invocation invocation;
#ifndef KALANDRA_ORDINARY_EXECUTION
      std::atomic<bool> token{false};
      invocation.bindCancellation(&token);
      invocation.allowNativeInterrupts(true);
#endif
#ifdef FIXED_ENTRY
      const auto value = ENTRY_NAME(&invocation);
#else
      const auto value = _mlir_ciface_kalandra_test_entry(seed, rounds, &invocation);
#endif
      if (invocation.status() || invocation.callDepth()) std::abort();
      invocation.settle();
      if (invocation.live() || invocation.allocations() != invocation.releases()) std::abort();
#else
      const auto value = runtime_workload(kind, seed, rounds);
#endif
      if (!std::isfinite(value) || value != expected) std::abort();
      checksum += value;
    }
    const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"round\":%lld,\"ns\":%.9g,\"checksum\":%.17g}\n", batch, ns, checksum);
  }
}
