#pragma once
#include "Runtime.h"
namespace kalandra::runtime {
// Private CPU value ABI. Nonempty values require an invocation-owned base.
struct StringDescriptor {
  void *allocated = nullptr;
  const char *data = nullptr;
  std::int64_t byteLength = 0;
};
bool validateString(Invocation &, const StringDescriptor &) noexcept;
} // namespace kalandra::runtime
extern "C" {
// Source bytes belong to a compiler-emitted constant global, not a user descriptor.
void kalandra_string_literal_v1(kalandra::runtime::Invocation *,
                                kalandra::runtime::StringDescriptor *, const char *,
                                std::uint64_t) noexcept;
void kalandra_string_validate_v1(kalandra::runtime::Invocation *,
                                 const kalandra::runtime::StringDescriptor *) noexcept;
}
// Private runtime entrypoints; descriptors must be readable, but their contents are untrusted.
extern "C" {
void kalandra_string_concat_v1(kalandra::runtime::Invocation *,
                               kalandra::runtime::StringDescriptor *,
                               const kalandra::runtime::StringDescriptor *,
                               const kalandra::runtime::StringDescriptor *) noexcept;
std::int64_t kalandra_string_equal_v1(kalandra::runtime::Invocation *,
                                      const kalandra::runtime::StringDescriptor *,
                                      const kalandra::runtime::StringDescriptor *) noexcept;
}
namespace kalandra::runtime {
// Both descriptor lengths and their sum must fit the String count domain.
inline std::optional<std::uint64_t> combinedStringBytes(std::uint64_t a, std::uint64_t b) noexcept {
  constexpr auto limit = std::uint64_t(INT64_MAX);
  if (a > limit || b > limit - a)
    return {};
  return a + b;
}
} // namespace kalandra::runtime
extern "C" {
std::int64_t kalandra_string_length_v1(kalandra::runtime::Invocation *,
                                       const kalandra::runtime::StringDescriptor *) noexcept;
std::int64_t kalandra_string_byte_length_v1(kalandra::runtime::Invocation *,
                                            const kalandra::runtime::StringDescriptor *) noexcept;
}
extern "C" void kalandra_string_index_v1(kalandra::runtime::Invocation *,
                                         kalandra::runtime::StringDescriptor *,
                                         const kalandra::runtime::StringDescriptor *,
                                         std::uint64_t) noexcept;
extern "C" void kalandra_string_slice_v1(kalandra::runtime::Invocation *,
                                         kalandra::runtime::StringDescriptor *,
                                         const kalandra::runtime::StringDescriptor *, std::uint64_t,
                                         std::uint64_t) noexcept;

// Retain a checked immutable backing before a task packet/result is published.
extern "C" bool kalandra_string_retain_v1(kalandra::runtime::Invocation *,
                                          const kalandra::runtime::Invocation *,
                                          const kalandra::runtime::StringDescriptor *) noexcept;

// Private recursive-box bridge. Inactive enum storage is never inspected.
extern "C" const void *
kalandra_recursive_string_dependency_v1(kalandra::runtime::Invocation *,
                                        const kalandra::runtime::StringDescriptor *,
                                        std::uint64_t active) noexcept;
