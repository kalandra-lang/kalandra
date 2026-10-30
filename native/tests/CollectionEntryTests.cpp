#include "../runtime/CollectionDisplay.h"
#include "../runtime/Tensor.h"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace kalandra;
using namespace kalandra::runtime;
constexpr auto element = static_cast<NumericType>(NUMERIC_TYPE);
using Cell = NumericValue<element>;
extern "C" {
void _mlir_ciface_kalandra_collection_entry(cli::CollectionDescriptor *, Invocation *);
extern const std::uint64_t kalandra_collection_rank_v1, kalandra_collection_element_v1,
    kalandra_collection_array_v1;
}
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **) {
  try {
    constexpr std::size_t rank = MAIN_ARRAY ? 1 : 2;
    require(kalandra_collection_rank_v1 == rank &&
                kalandra_collection_element_v1 == static_cast<unsigned>(element) &&
                kalandra_collection_array_v1 == MAIN_ARRAY,
            "collection entry metadata mismatch");
    const std::array<Cell, 4> expected{2, 4, 6, 8};
    std::uint64_t allocations = 0, cases = 0;
    for (unsigned attempt = 0; attempt <= allocations + 1; ++attempt) {
      Invocation context;
      context.failAllocationAt(attempt);
      struct Guarded {
        std::uint64_t before = 0x123456789abcdef0ULL;
        cli::CollectionDescriptor value;
        std::uint64_t after = 0xfedcba9876543210ULL;
      } output;
      std::memset(&output.value, 0xa5, sizeof(output.value));
      _mlir_ciface_kalandra_collection_entry(&output.value, &context);
      require(output.before == 0x123456789abcdef0ULL && output.after == 0xfedcba9876543210ULL,
              "collection entry overwrote descriptor guards");
      const auto bytes = std::as_bytes(std::span(&output.value, 1));
      constexpr auto activeBytes = 24 + 16 * rank;
      for (auto byte : bytes.subspan(activeBytes))
        require(byte == std::byte{0xa5}, "collection entry overwrote inactive descriptor suffix");
      std::optional<DisplayCollection> retained;
      if (argc > 1) {
        require(context.hasTaskError(), "invalid worker configuration did not reject admission");
      } else if (attempt && attempt <= allocations) {
        require(context.status() == 2, "collection entry lost allocation failure");
      } else {
        require(!context.status() && !context.hasTaskError(), "collection task entry failed");
        retained = retainForDisplay(context, output.value, rank, element, MAIN_ARRAY);
        require(bool(retained) && retained->count == 4,
                "collection entry did not return owned storage");
        require(output.value.dimensions[0] == (MAIN_ARRAY ? 4 : 2) &&
                    (MAIN_ARRAY || output.value.dimensions[1] == 2),
                "collection entry changed output shape");
      }
      if (!retained)
        for (auto byte : bytes.first(activeBytes))
          require(byte == std::byte{}, "failed collection entry exposed a partial descriptor");
      if (!attempt)
        allocations = context.allocations();
      require(allocations < 32, "unbounded collection entry allocations");
      context.settle();
      require(!context.live() && context.allocations() == context.releases(),
              "collection entry left live owners");
      if (retained)
        require(retained->data.size() == sizeof(expected) &&
                    std::memcmp(retained->data.data(), expected.data(), sizeof(expected)) == 0,
                "returned collection did not survive root cleanup");
      ++cases;
      if (argc > 1)
        break;
    }
    std::cout << "collection entry " << describeNumeric(element)->name << " array " << MAIN_ARRAY
              << ": " << cases << " checked ABI/ownership cases passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
