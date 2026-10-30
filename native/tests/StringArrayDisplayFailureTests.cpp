#include "../runtime/StringArray.h"
#include "kalandra/CollectionABI.h"
#include <cstring>
extern "C" {
extern const std::uint64_t kalandra_collection_rank_v1 = 1;
extern const std::uint64_t kalandra_collection_element_v1 = kalandra::cli::stringArrayElement;
extern const std::uint64_t kalandra_collection_array_v1 = 1;
void _mlir_ciface_kalandra_collection_entry(kalandra::cli::CollectionDescriptor *out,
                                            kalandra::runtime::Invocation *context) {
  using namespace kalandra::runtime;
  StringDescriptor first, bad;
  kalandra_string_literal_v1(context, &first, "first", 5);
  kalandra_string_literal_v1(context, &bad, "bad", 3);
  if (context->status())
    return;
  // The malformed tail must be rejected even when --limit hides every element.
  bad.byteLength = 4;
  auto *storage = context->allocateRecord(2 * sizeof(StringDescriptor));
  if (!storage)
    return;
  std::memcpy(storage, &first, sizeof(first));
  std::memcpy(static_cast<std::byte *>(storage) + sizeof(first), &bad, sizeof(bad));
  out->allocated = out->aligned = storage;
  out->dimensions[0] = 2;
  out->dimensions[1] = 1;
}
}
