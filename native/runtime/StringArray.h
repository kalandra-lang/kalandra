#pragma once
#include "String.h"
namespace kalandra::runtime {
// Private rank-one contiguous descriptor. The backing contains StringDescriptors.
struct StringArrayDescriptor {
  void *allocated = nullptr, *data = nullptr;
  std::int64_t offset = 0, size = 0, stride = 0;
};
} // namespace kalandra::runtime
extern "C" {
void kalandra_string_array_validate_v1(kalandra::runtime::Invocation *,
                                       const kalandra::runtime::StringArrayDescriptor *) noexcept;
bool kalandra_string_array_retain_v1(kalandra::runtime::Invocation *,
                                     const kalandra::runtime::Invocation *,
                                     const kalandra::runtime::StringArrayDescriptor *) noexcept;
}

// Private recursive-box bridge. Returns a managed owner for the array and its String buffers.
extern "C" const void *
kalandra_recursive_string_array_dependency_v1(kalandra::runtime::Invocation *,
                                              const kalandra::runtime::StringArrayDescriptor *,
                                              std::uint64_t active) noexcept;
