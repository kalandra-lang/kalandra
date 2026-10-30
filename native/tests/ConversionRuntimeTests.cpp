#include "../metal-proof/Float64Oracle.h"
#include "../runtime/NumericStorage.h"
#include "../runtime/Runtime.h"
#include "llvm/ADT/APSInt.h"
#include <bit>
#include <cmath>
#include <iostream>
using namespace kalandra;
using namespace kalandra::runtime;
using namespace float64_proof;
#ifndef SOURCE_TYPE
#define SOURCE_TYPE 9
#endif
#ifndef DESTINATION_TYPE
#define DESTINATION_TYPE 6
#endif
constexpr auto source = static_cast<NumericType>(SOURCE_TYPE),
               destination = static_cast<NumericType>(DESTINATION_TYPE);
using Input = NumericValue<source>;
using Output = NumericValue<destination>;
extern "C" Output _mlir_ciface_kalandra_test_entry(Input, Invocation *);
#include "ConversionOracle.h"
int main() {
  try {
    auto inputs = conversionInputs();
    std::vector<Expected> expected;
    for (auto bits : inputs)
      expected.push_back(reference(bits));
    std::uint64_t cases = 0, faults = 0;
    {
      FpState state;
      const unsigned environments =
#ifdef TOTAL_HARDWARE_CAST
          1;
#else
          8;
#endif
      for (unsigned environment = 0; environment < environments; ++environment) {
        state.set(environment);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
          Invocation context;
          auto value = _mlir_ciface_kalandra_test_entry(std::bit_cast<Input>(Raw<Input>(inputs[i])),
                                                        &context);
          auto bits = std::uint64_t(std::bit_cast<Raw<Output>>(value));
          const auto valid = expected[i].valid;
          bool sameBits = bits == (valid ? expected[i].bits : 0);
#ifdef TOTAL_HARDWARE_CAST
          // Hardware widening preserves NaN as a value; payload preservation is
          // deliberately outside the language contract. All other bits match.
          if constexpr (source == NumericType::Float32 && destination == NumericType::Float64)
            sameBits = sameBits || (std::isnan(std::bit_cast<Input>(Raw<Input>(inputs[i]))) &&
                                    std::isnan(value));
#endif
          if (context.status() != (valid ? 0 : 4) || !sameBits ||
              (!valid && (context.line() != 2 || context.column() != 3))) {
            std::cerr << describeNumeric(source)->name << " -> "
                      << describeNumeric(destination)->name << " input " << std::hex << inputs[i]
                      << " result " << bits << " expected " << expected[i].bits << std::dec
                      << " status " << context.status() << " valid " << valid << '\n';
            throw std::runtime_error("conversion mismatch");
          }
          ++cases;
          faults += !valid;
          context.settle();
          if (context.live() || context.allocations() != context.releases())
            throw std::runtime_error("conversion leaked");
        }
#ifndef TOTAL_HARDWARE_CAST
        state.checkFlags();
#endif
      }
    }
    Invocation context;
    std::atomic<bool> token{true};
    context.bindCancellation(&token);
    auto value = _mlir_ciface_kalandra_test_entry(Input{}, &context);
    if (std::bit_cast<Raw<Output>>(value) != 0 || context.poll() != -1 || context.status())
      throw std::runtime_error("conversion cancellation failed");
    context.settle();
    std::cout << "conversion " << describeNumeric(source)->name << " -> "
              << describeNumeric(destination)->name << ": " << cases << " cases, " << faults
              << " faults; APFloat oracle and settled owners passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
