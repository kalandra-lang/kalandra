#include "IntegerOracle.h"
#include <cstdio>
using namespace integer_proof;
int main() {
  std::uint64_t comparisons = 0, faults = 0;
  for (const auto &entry : cases)
    for (const auto pair : corpus(entry.type))
      for (unsigned op = 0; op < 6; ++op) {
        const auto expected = oracle(entry.type, pair, op);
        const auto valid = entry.valid[op](pair.a, pair.b);
        const auto bits = entry.results[op](pair.a, pair.b);
        if (valid != unsigned(expected.valid) || (valid && bits != expected.bits)) {
          std::fprintf(stderr,
                       "Checked integer mismatch %s/%u a=%016llx b=%016llx bits=%016llx "
                       "expected=%016llx valid=%u/%u\n",
                       entry.name, op, (unsigned long long)pair.a, (unsigned long long)pair.b,
                       (unsigned long long)bits, (unsigned long long)expected.bits, valid,
                       unsigned(expected.valid));
          return 1;
        }
        ++comparisons;
        faults += !valid;
      }
  std::printf("Checked GPU integer algorithms on CPU: %llu exact APInt comparisons, %llu "
              "arithmetic faults, eight widths passed\n",
              (unsigned long long)comparisons, (unsigned long long)faults);
}
