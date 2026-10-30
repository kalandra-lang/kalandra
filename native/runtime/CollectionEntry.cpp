#include "CollectionDisplay.h"
#include <charconv>
#include <cstdlib>
#include <string_view>
extern "C" {
void _mlir_ciface_kalandra_collection_entry(kalandra::cli::CollectionDescriptor *,
                                            kalandra::runtime::Invocation *);
extern const std::uint64_t kalandra_collection_rank_v1;
extern const std::uint64_t kalandra_collection_element_v1;
extern const std::uint64_t kalandra_collection_array_v1;
}
int main(int argc, char **argv) {
  std::uint64_t limit = 100;
  bool valid = argc == 1;
  if (argc == 2 && std::string_view(argv[1]) == "--all") {
    limit = UINT64_MAX;
    valid = true;
  } else if (argc == 3 && std::string_view(argv[1]) == "--limit") {
    const std::string_view text(argv[2]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
    valid = !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
  }
  if (!valid) {
    std::fprintf(stderr, "usage: %s [--all | --limit <nonnegative element count>]\n", argv[0]);
    return 2;
  }
  using namespace kalandra;
  using namespace kalandra::runtime;
  Invocation context;
  if (kalandra_collection_rank_v1 > cli::maximumCollectionRank ||
      (kalandra_collection_element_v1 > static_cast<unsigned>(NumericType::Float64) &&
       kalandra_collection_element_v1 != cli::boolArrayElement) ||
      kalandra_collection_array_v1 > 1) {
    context.fault(3);
    context.reportFailure();
  }
  const auto rank = std::size_t(kalandra_collection_rank_v1);
  const auto element = static_cast<NumericType>(kalandra_collection_element_v1);
  const bool array = kalandra_collection_array_v1;
  const bool boolean = kalandra_collection_element_v1 == cli::boolArrayElement;
  if ((boolean ? (!array || rank != 1)
               : !(array ? numericCollectionStorageAvailable(element, NumericTarget::Cpu)
                         : numericTensorElementwiseAvailable(element, NumericTarget::Cpu))) ||
      (array && rank != 1)) {
    context.fault(3);
    context.reportFailure();
  }
  cli::CollectionDescriptor descriptor;
  _mlir_ciface_kalandra_collection_entry(&descriptor, &context);
  if (context.status() || context.hasExpectedError())
    context.reportFailure();
  auto retained = boolean ? retainBoolForDisplay(context, descriptor)
                          : retainForDisplay(context, descriptor, rank, element, array);
  if (!retained) {
    context.fault(3);
    context.reportFailure();
  }
  context.settle();
  if (context.live() || context.allocations() != context.releases()) {
    std::fprintf(stderr, "collection display failed to settle result owners\n");
    return 1;
  }
  if (std::getenv("KALANDRA_TEST_REPORT_CLEANUP"))
    std::fprintf(stderr, "cleanup: live=%llu allocations=%llu releases=%llu\n",
                 static_cast<unsigned long long>(context.live()),
                 static_cast<unsigned long long>(context.allocations()),
                 static_cast<unsigned long long>(context.releases()));
  if (!(boolean ? printBoolArray(stdout, *retained, limit)
                : printCollection(stdout, *retained, {descriptor.dimensions.data(), rank}, element,
                                  array, limit))) {
    std::fprintf(stderr, "cannot write collection result\n");
    return 1;
  }
}
