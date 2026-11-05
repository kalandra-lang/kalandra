#pragma once
#include "kalandra/Continuation.h"
#include <cstdint>
#include <span>

namespace kalandra {
// Invocation-frame roots before each instruction, including allocation operands
// and excluding an overwritten destination unless its previous value is read.
// No device liveness decisions are delegated to the runtime/host scheduler.
std::expected<std::vector<std::vector<continuation::Slot>>, std::string>
deviceLiveRoots(const continuation::Plan &, std::span<const continuation::Slot> tracked,
                std::uint64_t maxBytes = 64 * 1024 * 1024, std::uint64_t maxWork = 30000000,
                bool callFrames = false);
// The caller's result slot is undefined until the callee returns. Only live
// continuation values other than that slot must survive while suspended.
std::expected<std::vector<continuation::Slot>, std::string>
deviceSuspendedCallRoots(const continuation::Plan &, std::size_t call,
                         std::span<const std::vector<continuation::Slot>> live);
} // namespace kalandra
