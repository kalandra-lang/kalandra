#include "Float64Oracle.h"
extern "C" std::uint64_t strict64_add(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_mul(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_multiply_add(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_sub(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_neg(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_div(std::uint64_t, std::uint64_t);
extern "C" std::uint64_t strict64_association(std::uint64_t, std::uint64_t, std::uint64_t);
using namespace float64_proof;
using Operation = Bits (*)(Bits, Bits);
constexpr Operation Operations[] = {strict64_add, strict64_mul, strict64_multiply_add,
                                    strict64_sub, strict64_neg, strict64_div};

int main() {
  try {
    auto pairs = corpus();
    std::vector<Results> expected;
    for (auto pair : pairs) {
      Results result;
      for (unsigned op = 0; op < 6; ++op)
        result[op] = oracle(pair, op);
      expected.push_back(result);
    }
    std::uint64_t comparisons = 0;
    FpState state;
    for (unsigned mode = 0; mode < 8; ++mode) {
      state.set(mode);
      for (std::size_t i = 0; i < pairs.size(); ++i)
        for (unsigned op = 0; op < 6; ++op) {
          const auto actual = Operations[op](pairs[i].a, pairs[i].b);
          if (actual != expected[i][op]) {
            std::fprintf(stderr,
                         "binary64 mismatch mode=%u op=%u a=%016llx b=%016llx expected=%016llx "
                         "actual=%016llx\n",
                         mode, op, (unsigned long long)pairs[i].a, (unsigned long long)pairs[i].b,
                         (unsigned long long)expected[i][op], (unsigned long long)actual);
            return 1;
          }
          ++comparisons;
        }
      // (2^53 + 1) - 2^53 must round at the first addition; reassociation produces 1.
      if (strict64_association(0x4340000000000000, 0x3ff0000000000000, 0xc340000000000000) != 0)
        throw std::runtime_error("addition was reassociated");
      state.checkFlags();
    }
    std::printf("Strict binary64 CPU: %zu pairs, %llu exact APFloat comparisons, 8 FP "
                "environments; add/subtract/multiply/divide/negate, separate rounding, signed "
                "zero, canonical arithmetic NaN and exact negation payloads passed\n",
                pairs.size(), (unsigned long long)comparisons);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
