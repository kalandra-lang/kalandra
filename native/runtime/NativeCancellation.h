#pragma once
#include <cstdint>

namespace kalandra::runtime {
// Private scheduler capability. False keeps the cooperative generated workers.
// Initialization never replaces an existing signal owner. Once acquired, the
// runtime owns its private interrupt signal until process exit.
bool initializeNativeCancellation() noexcept;
bool nativeCancellationCanAbandon() noexcept;
std::uintptr_t nativeCancellationThread() noexcept;
void interruptNativeCancellation(std::uintptr_t thread) noexcept;
// Private qualification counter; increments only on an actual PC redirect.
std::uint64_t nativeCancellationRedirects() noexcept;
}
