#include "../metal-proof/Float64Oracle.h"
#include "../runtime/Tensor.h"
#include <bit>
#include <cstring>
#include <iostream>
#include <optional>
#include <type_traits>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
using Raw = std::conditional_t<sizeof(Cell) == 8, std::uint64_t, std::uint32_t>;
using Tensor = CpuTensor<element, 1>;
extern "C" void _mlir_ciface_kalandra_test_entry(Tensor *, Tensor *, Invocation *);
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main() {
  try {
    std::uint64_t cases = 0, comparisons = 0, seed = 0x9e3779b97f4a7c15ULL;
    FpState fp;
    for (unsigned environment = 0; environment < 8; ++environment) {
      fp.set(environment);
      for (std::size_t length : {0u, 1u, 17u, 4096u}) {
        std::vector<Cell> backing(2 * length + 8);
        Tensor input{};
        input.allocated = backing.data();
        input.aligned = backing.data() + 1;
        input.offset = 2;
        input.sizes = {std::int64_t(length)};
        input.strides = {2};
        for (std::size_t i = 0; i < length; ++i) {
          seed ^= seed << 13;
          seed ^= seed >> 7;
          seed ^= seed << 17;
          Raw bits = Raw(seed);
          if (i < 8) {
            constexpr unsigned width = sizeof(Cell) * 8;
            constexpr Raw exponent =
                sizeof(Cell) == 8 ? Raw(0x7ff0000000000000ULL) : Raw(0x7f800000);
            const Raw edges[] = {0,
                                 Raw(Raw(1) << (width - 1)),
                                 1,
                                 Raw(~Raw(0)),
                                 exponent,
                                 Raw(exponent | 1),
                                 Raw(exponent | (Raw(1) << (width - 2))),
                                 Raw(~(Raw(1) << (width - 1)))};
            bits = edges[i];
          }
          backing[3 + 2 * i] = std::bit_cast<Cell>(bits);
        }
        auto snapshot = backing;
        std::uint64_t allocations = 0;
        for (unsigned fail = 0; fail <= allocations + 1; ++fail) {
          Invocation context;
          context.failAllocationAt(fail);
          Tensor output{};
          _mlir_ciface_kalandra_test_entry(&output, &input, &context);
          ++cases;
          const bool failure = fail && fail <= allocations;
          require(context.status() == (failure ? 2 : 0), "map allocation status mismatch");
          auto verify = [&] {
            require(output.sizes[0] == std::int64_t(length), "map changed array length");
            for (std::size_t i = 0; i < length; ++i) {
              require(std::bit_cast<Raw>(output.aligned[output.offset + i * output.strides[0]]) ==
                          std::bit_cast<Raw>(backing[3 + 2 * i]),
                      "map changed identity bits/order");
              ++comparisons;
            }
            require(!std::memcmp(backing.data(), snapshot.data(), backing.size() * sizeof(Cell)),
                    "map mutated borrowed input");
          };
          std::optional<AllocationLease> retained;
          if (failure)
            require(!output.allocated && !output.aligned, "failed map published partial result");
          else {
            verify();
            if (length) {
              retained = context.lease(output.allocated);
              require(bool(retained), "mapped output not owned");
            }
          }
          if (!fail) {
            allocations = context.allocations();
            require(allocations < 16, "unbounded map allocation baseline");
          }
          context.settle();
          require(!context.live() && context.allocations() == context.releases(),
                  "map owners leaked");
          if (!failure)
            verify();
        }
      }
      fp.checkFlags();
    }
    std::cout << "map identity " << describeNumeric(element)->name << ": " << cases
              << " invocations, " << comparisons
              << " exact lanes; strided inputs immutable and retained outputs passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
