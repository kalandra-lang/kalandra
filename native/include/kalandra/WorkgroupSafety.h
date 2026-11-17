#pragma once
#include "kalandra/NumericType.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kalandra {
// Compiler proof input after specializing local indices and collective control.
// Unknown accesses/control must be diagnosed before constructing this plan;
// selecting one possible path is not proof of the other paths.
struct WorkgroupScratch {
  NumericType element;
  std::uint64_t elements;
};
struct WorkgroupAccess {
  std::size_t scratch;
  std::uint64_t index;
  bool write;
  // A possible write conflicts with other lanes, but only a definite write can
  // establish initialization. Reads are checked even on a possible path.
  bool definite = true;
};
struct WorkgroupPhase {
  std::vector<WorkgroupAccess> accesses;
  // Identity of the collective call site, not merely a count of barriers.
  std::optional<std::size_t> barrier = {};
};
struct WorkgroupLane {
  std::vector<WorkgroupPhase> phases;
};
enum class WorkgroupProofFailure {
  InvalidPlan,
  Budget,
  Bounds,
  DivergentBarrier,
  WriteRace,
  ReadRace,
  UninitializedRead
};
struct WorkgroupProofError {
  WorkgroupProofFailure kind;
  std::size_t lane = 0, phase = 0, access = 0;
  std::string message;
};
struct WorkgroupProofLimits {
  std::size_t lanes = 4096, accesses = 1000000, phases = 4096;
  std::uint64_t scratchElements = 1000000;
};
struct WorkgroupProof {
  std::size_t lanes, phases, accesses;
  std::uint64_t scratchBytes;
};
// Narrow values retain exact bits in 32-bit shared carriers. This avoids
// requiring optional physical 8/16-bit workgroup storage capabilities.
constexpr unsigned workgroupCarrierBytes(NumericType type) {
  const auto description = describeNumeric(type);
  return description ? (description->bits <= 32 ? 4 : 8) : 0;
}
std::expected<WorkgroupProof, WorkgroupProofError>
proveWorkgroupSafety(std::span<const WorkgroupScratch>, std::span<const WorkgroupLane>,
                     const WorkgroupProofLimits & = {});
} // namespace kalandra
