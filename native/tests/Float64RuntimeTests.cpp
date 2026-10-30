#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Runtime.h"
#include <bit>
#include <cstdlib>
#include <cstring>
extern "C" double _mlir_ciface_kalandra_test_entry(double, double, kalandra::runtime::Invocation *);
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  unsigned op = std::strtoul(argv[1], nullptr, 10);
  if (op > 5)
    return 2;
  try {
    using namespace float64_proof;
    const auto pairs = corpus();
    std::vector<Bits> expected;
    for (auto pair : pairs)
      expected.push_back(oracle(pair, op));
    FpState state;
    kalandra::runtime::Invocation invocation;
    std::uint64_t comparisons = 0;
    for (unsigned mode = 0; mode < 8; ++mode) {
      state.set(mode);
      for (std::size_t i = 0; i < pairs.size(); ++i) {
        auto actual = std::bit_cast<Bits>(_mlir_ciface_kalandra_test_entry(
            std::bit_cast<double>(pairs[i].a), std::bit_cast<double>(pairs[i].b), &invocation));
        if (actual != expected[i] || invocation.status() || invocation.live()) {
          std::fprintf(stderr,
                       "source float64 mismatch op=%u mode=%u a=%016llx b=%016llx expected=%016llx "
                       "actual=%016llx\n",
                       op, mode, (unsigned long long)pairs[i].a, (unsigned long long)pairs[i].b,
                       (unsigned long long)expected[i], (unsigned long long)actual);
          return 1;
        }
        ++comparisons;
      }
      state.checkFlags();
    }
    std::atomic<bool> cancelled{true};
    invocation.bindCancellation(&cancelled);
    if (_mlir_ciface_kalandra_test_entry(1, 2, &invocation) != 0 || invocation.poll() != -1)
      throw std::runtime_error("cancelled invocation executed source arithmetic");
    invocation.settle();
    if (invocation.live() || invocation.allocations() != invocation.releases())
      throw std::runtime_error("source float64 leaked allocations");
    std::printf("Source float64 op %u: %zu pairs, %llu exact APFloat comparisons, 8 FP "
                "environments and cancelled invocation gate passed\n",
                op, pairs.size(), (unsigned long long)comparisons);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
