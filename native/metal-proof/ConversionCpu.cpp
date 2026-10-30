#include "ConversionOracle.h"
#include <cstdio>
using namespace conversion_proof;
int main() {
  std::uint64_t comparisons = 0, faults = 0;
  for (const auto &source : cases) {
    auto inputs = corpus(source.type);
    for (unsigned destination = 0; destination < numericTypes.size(); ++destination)
      for (auto input : inputs) {
        auto expected = oracle(source.type, numericTypes[destination].type, input);
        auto valid = source.valid(input, destination);
        auto bits = source.bits(input, destination);
        if (valid != unsigned(expected.valid) || (valid && bits != expected.bits)) {
          std::fprintf(
              stderr,
              "GPU conversion mismatch %s -> %s input=%016llx bits=%016llx/%016llx valid=%u/%u\n",
              source.name, numericTypes[destination].name.data(), (unsigned long long)input,
              (unsigned long long)bits, (unsigned long long)expected.bits, valid,
              unsigned(expected.valid));
          return 1;
        }
        ++comparisons;
        faults += !valid;
      }
  }
  std::printf("Compact GPU conversions: %llu exact APFloat/APInt comparisons, %llu checked faults, "
              "all 100 type pairs passed\n",
              (unsigned long long)comparisons, (unsigned long long)faults);
}
