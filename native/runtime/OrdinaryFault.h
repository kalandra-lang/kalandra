#pragma once
#include <bit>
#include <cstdint>
#include <type_traits>

namespace kalandra::runtime {
// Private compiler/host ABI for a closed ordinary scalar entry with no escaping resource.
// The caller need not initialize this buffer. It is defined on every ambiguous
// result (the single high-bit integer pattern / any floating NaN); read it only through this
// predicate. Failure returns that integer pattern/quiet NaN and its original first-fault
// code/location. Successful ambiguous values retain every bit and publish code zero.
struct OrdinaryFault {
  std::int64_t code, line, column;
};
template <class T> inline bool ordinaryAmbiguous(T value) noexcept {
  if constexpr (std::is_floating_point_v<T>) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    const auto bits = std::bit_cast<Bits>(value);
    constexpr Bits exponent = sizeof(T) == 8 ? Bits(0x7ff0000000000000ULL) : Bits(0x7f800000U);
    constexpr Bits fraction = sizeof(T) == 8 ? Bits(0x000fffffffffffffULL) : Bits(0x007fffffU);
    return (bits & exponent) == exponent && (bits & fraction) != 0;
  } else {
    static_assert(std::is_integral_v<T>);
    if constexpr (std::is_same_v<T, bool>)
      return value;
    else {
      using Bits = std::make_unsigned_t<T>;
      return std::bit_cast<Bits>(value) == Bits(Bits(1) << (sizeof(T) * 8 - 1));
    }
  }
}
template <class T> inline bool ordinaryFaulted(T value, const OrdinaryFault &fault) noexcept {
  return ordinaryAmbiguous(value) && fault.code != 0;
}
static_assert(sizeof(OrdinaryFault) == 24);
} // namespace kalandra::runtime
