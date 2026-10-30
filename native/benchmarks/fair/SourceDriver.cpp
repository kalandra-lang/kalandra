#include "../../runtime/Runtime.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#ifdef KALANDRA_KERNEL
extern "C" double _mlir_ciface_kalandra_test_entry(std::int64_t, std::int64_t,
                                                   kalandra::runtime::Invocation *);
#else
extern "C" double source_kernel(int, std::int64_t, std::int64_t);
#endif
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  const int kind = std::stoi(argv[1]), depth = std::stoi(argv[2]);
  const auto iterations = std::stoull(argv[3]);
  const int rounds = std::stoi(argv[4]);
  const auto seed = std::stoll(argv[5]);
  if (kind < 0 || kind > 2 || depth < 0 || depth > 14 || seed < 1 || seed > 31 || !iterations ||
      rounds < 1)
    return 2;
  for (int round = -2; round < rounds; ++round) {
    double checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
#ifdef KALANDRA_KERNEL
      kalandra::runtime::Invocation context;
      checksum += _mlir_ciface_kalandra_test_entry(seed, depth, &context);
      if (context.status())
        throw std::runtime_error("Kalandra source kernel failed");
      context.settle();
      if (context.live())
        throw std::runtime_error("source kernel leaked");
#else
      checksum += source_kernel(kind, seed, depth);
#endif
    }
    const auto ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("{\"round\":%d,\"ns\":%.0f,\"checksum\":%.17g}\n", round, ns, checksum);
  }
}
