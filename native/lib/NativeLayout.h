#pragma once
namespace kalandra {
// Queried from the pinned native Clang at configuration. Record allocation,
// stack admission and task transport must share the actual target ABI.
inline constexpr char nativeCpuDataLayout[] = KALANDRA_NATIVE_CPU_DATA_LAYOUT;
} // namespace kalandra
