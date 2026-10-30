#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <iostream>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
using Tensor = CpuTensor<NumericType::Float64, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, bool, Invocation *);
int main() {
  try {
    unsigned cases = 0, checks = 0;
    FpState state;
    for (unsigned environment = 0; environment < 8; ++environment) {
      state.set(environment);
      for (bool flag : {false, true}) {
        const auto expected = flag ? std::vector<Bits>{Sign, UINT64_C(0x3ff0000000000001)}
                                   : std::vector<Bits>{1, UINT64_C(0x7fefffffffffffff), 0};
        std::uint64_t allocations = 0;
        for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
          Invocation context;
          context.failAllocationAt(fail);
          Tensor result{};
          _mlir_ciface_kalandra_test_entry(&result, flag, &context);
          ++cases;
          const bool invalid = fail && fail <= allocations;
          if (context.status() != (invalid ? 2 : 0))
            throw std::runtime_error("conditional tensor allocation status mismatch");
          std::optional<AllocationLease> retained;
          auto verify = [&] {
            if (!result.aligned || result.sizes[0] != std::int64_t(expected.size()))
              throw std::runtime_error("conditional tensor shape mismatch");
            for (std::size_t i = 0; i < expected.size(); ++i) {
              if (std::bit_cast<Bits>(result.aligned[result.offset + i * result.strides[0]]) !=
                  expected[i])
                throw std::runtime_error("conditional tensor bits mismatch");
              ++checks;
            }
          };
          if (invalid) {
            if (result.allocated)
              throw std::runtime_error("partial conditional tensor published");
          } else {
            verify();
            retained = context.lease(result.allocated);
            if (!retained)
              throw std::runtime_error("conditional tensor not owned");
          }
          if (!fail) {
            allocations = context.allocations();
            if (!allocations || allocations > 8)
              throw std::runtime_error("invalid conditional allocation baseline");
          }
          context.settle();
          if (context.live() || context.allocations() != context.releases())
            throw std::runtime_error("conditional tensor owners leaked");
          if (!invalid)
            verify();
        }
      }
      state.checkFlags();
    }
    std::cout << "conditional tensor: " << cases << " cases, " << checks
              << " exact retained lane checks; dynamic shape joins, allocation failures and "
                 "settled owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
