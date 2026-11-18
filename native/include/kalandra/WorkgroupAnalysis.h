#pragma once
#include "kalandra/Semantics.h"
#include "kalandra/WorkgroupSafety.h"
#include <expected>

namespace kalandra {
struct WorkgroupAnalysisLimits {
  WorkgroupProofLimits proof;
  std::size_t steps = 1000000, uniformPaths = 64, callDepth = 256, loopIterations = 4096;
};
struct WorkgroupAnalysis {
  std::vector<WorkgroupScratch> scratch;
  std::vector<ValueId> scratchSites;
  std::vector<ValueId> barrierSites;
  std::size_t uniformPaths = 0;
  std::uint64_t scratchBytes = 0;
};
// Validate every participating lane and every admitted control alternative.
// This is an admission proof, not GPU execution or an arithmetic interpreter.
std::expected<WorkgroupAnalysis, Diagnostic>
analyzeWorkgroup(const TypedModule &, ValueId dispatch, const WorkgroupAnalysisLimits & = {});
} // namespace kalandra
