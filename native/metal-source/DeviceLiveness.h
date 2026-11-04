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
                std::uint64_t maxBytes = 64 * 1024 * 1024, std::uint64_t maxWork = 30000000);
} // namespace kalandra
