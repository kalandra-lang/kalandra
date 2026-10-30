#include "StringArray.h"
#include <cstring>
extern "C" void
kalandra_string_array_validate_v1(kalandra::runtime::Invocation *context,
                                  const kalandra::runtime::StringArrayDescriptor *value) noexcept {
  using namespace kalandra::runtime;
  if (context->poll() != 0)
    return;
  kalandra_record_array_validate_v1(context, value, sizeof(StringDescriptor),
                                    alignof(StringDescriptor));
  if (context->poll() != 0 || !context->usable())
    return;
  auto lease = value->allocated ? context->lease(value->allocated) : std::nullopt;
  if (value->size && !lease) {
    context->fault(3);
    return;
  }
  for (std::int64_t i = 0; i < value->size; ++i) {
    StringDescriptor cell;
    std::memcpy(&cell, lease->bytes().data() + i * sizeof(cell), sizeof(cell));
    if (!validateString(*context, cell))
      return;
  }
}
extern "C" bool
kalandra_string_array_retain_v1(kalandra::runtime::Invocation *destination,
                                const kalandra::runtime::Invocation *source,
                                const kalandra::runtime::StringArrayDescriptor *value) noexcept {
  using namespace kalandra::runtime;
  if (destination->poll() != 0 || !destination->usable())
    return false;
  if (!kalandra_record_array_retain_v1(destination, source, value, sizeof(StringDescriptor),
                                       alignof(StringDescriptor)))
    return false;
  auto lease = value->allocated ? source->lease(value->allocated) : std::nullopt;
  if (value->size && !lease) {
    destination->fault(3);
    return false;
  }
  for (std::int64_t i = 0; i < value->size; ++i) {
    StringDescriptor cell;
    std::memcpy(&cell, lease->bytes().data() + i * sizeof(cell), sizeof(cell));
    if (!kalandra_string_retain_v1(destination, source, &cell))
      return false;
  }
  return destination->poll() == 0;
}

extern "C" const void *
kalandra_recursive_string_array_dependency_v1(kalandra::runtime::Invocation *context,
                                              const kalandra::runtime::StringArrayDescriptor *value,
                                              std::uint64_t active) noexcept {
  using namespace kalandra::runtime;
  if (context->poll() != 0 || !context->usable())
    return nullptr;
  if (active > 1 || (active && !value)) {
    context->fault(3);
    return nullptr;
  }
  if (!active)
    return nullptr;
  kalandra_string_array_validate_v1(context, value);
  if (context->poll() != 0 || !context->usable() || !value->size)
    return nullptr;
  const auto array = *value;
  auto backing = context->lease(array.allocated);
  if (!backing) {
    context->fault(3);
    return nullptr;
  }
  std::size_t count = 1; // Descriptor storage is also owned.
  for (std::int64_t i = 0; i < array.size; ++i) {
    if (context->poll() != 0)
      return nullptr;
    StringDescriptor cell;
    std::memcpy(&cell, backing->bytes().data() + i * sizeof(cell), sizeof(cell));
    if (cell.allocated && ++count > Invocation::maxRecursiveDependencies) {
      context->fault(2);
      return nullptr;
    }
  }
  auto **dependencies = static_cast<const void **>(context->allocateRecord(count * sizeof(void *)));
  if (!dependencies)
    return nullptr;
  struct Temporary {
    Invocation *context;
    void *storage;
    ~Temporary() { context->release(storage); }
  } temporary{context, dependencies};
  dependencies[0] = array.allocated;
  std::size_t index = 1;
  for (std::int64_t i = 0; i < array.size; ++i) {
    if (context->poll() != 0)
      return nullptr;
    StringDescriptor cell;
    std::memcpy(&cell, backing->bytes().data() + i * sizeof(cell), sizeof(cell));
    if (cell.allocated)
      dependencies[index++] = cell.allocated;
  }
  // Invocation access is exclusive and source storage immutable throughout these passes.
  static const char key = 0;
  return context->makeRecursiveValueFromPointers(std::as_bytes(std::span{&array, 1}), &key,
                                                 {dependencies, count});
}
