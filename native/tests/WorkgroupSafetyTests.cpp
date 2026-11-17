#include "kalandra/WorkgroupSafety.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace kalandra;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "workgroup safety assertion %d: %s\n", __LINE__, #condition);             \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)

static void rejected(const std::vector<WorkgroupScratch> &scratch,
                     const std::vector<WorkgroupLane> &lanes, WorkgroupProofFailure kind) {
  const auto result = proveWorkgroupSafety(scratch, lanes);
  CHECK(!result && result.error().kind == kind);
}

int main() {
  using Failure = WorkgroupProofFailure;
  // The accepted halo stencil: 256 exclusive middle writes, two exclusive halo
  // writes, then overlapping read-only windows after one collective.
  std::vector<WorkgroupScratch> scratch{{NumericType::Float32, 258}};
  std::vector<WorkgroupLane> stencil(256);
  for (std::size_t lane = 0; lane < stencil.size(); ++lane) {
    auto &trace = stencil[lane].phases;
    trace = {{{{0, lane + 1, true}}, 7},
             {{{0, lane, false}, {0, lane + 1, false}, {0, lane + 2, false}}, {}}};
    if (!lane)
      trace[0].accesses.push_back({0, 0, true});
    if (lane + 1 == stencil.size())
      trace[0].accesses.push_back({0, 257, true});
  }
  auto proof = proveWorkgroupSafety(scratch, stencil);
  CHECK(proof && proof->lanes == 256 && proof->phases == 2 && proof->scratchBytes == 1032);
  auto broken = stencil;
  broken[255].phases[0].accesses.pop_back();
  rejected(scratch, broken, Failure::UninitializedRead);
  broken = stencil;
  broken[0].phases[0].accesses.back().definite = false;
  rejected(scratch, broken, Failure::UninitializedRead);
  broken = stencil;
  broken[1].phases[0].accesses[0].index = 1;
  rejected(scratch, broken, Failure::WriteRace);
  broken = stencil;
  broken[255].phases[0].accesses.back().index = 258;
  rejected(scratch, broken, Failure::Bounds);
  broken = stencil;
  broken[1].phases[0].barrier = 8;
  rejected(scratch, broken, Failure::DivergentBarrier);
  broken = stencil;
  broken[1].phases.pop_back();
  rejected(scratch, broken, Failure::DivergentBarrier);

  // The usual halving reduction needs only one barrier per level: partners
  // are inactive at that level, so no other lane overwrites the values read.
  std::vector<WorkgroupLane> reduction(8);
  for (std::size_t lane = 0; lane < reduction.size(); ++lane) {
    auto &trace = reduction[lane].phases;
    trace.push_back({{{0, lane, true}}, 1});
    std::size_t site = 2;
    for (std::size_t stride = 4; stride; stride /= 2) {
      WorkgroupPhase level{{}, site++};
      if (lane < stride)
        level.accesses = {{0, lane, false}, {0, lane + stride, false}, {0, lane, true}};
      trace.push_back(level);
    }
    trace.push_back({{{0, 0, false}}, {}});
  }
  CHECK(proveWorkgroupSafety({scratch.data(), 1}, reduction));

  // Read/write races are rejected in either lane traversal order, including
  // when storage was already initialized at a previous barrier.
  for (bool reverse : {false, true}) {
    std::vector<WorkgroupLane> lanes(2);
    lanes[0].phases = {{{{0, 0, true}}, 1}, {{{0, 0, false}}, {}}};
    lanes[1].phases = {{{}, 1}, {{{0, 0, true}}, {}}};
    if (reverse)
      std::swap(lanes[0], lanes[1]);
    rejected(scratch, lanes, Failure::ReadRace);
  }
  // Exhaust the tiny two-lane read/write space. No initial shared contents
  // exist. Only a lane's own definite prior write can justify a same-phase read.
  for (unsigned writeMask = 0; writeMask < 4; ++writeMask)
    for (unsigned a = 0; a < 2; ++a)
      for (unsigned b = 0; b < 2; ++b) {
        std::vector<WorkgroupLane> lanes(2);
        lanes[0].phases = {{{{0, a, true, bool(writeMask & 1)}, {0, a, false}}, {}}};
        lanes[1].phases = {{{{0, b, true, bool(writeMask & 2)}, {0, b, false}}, {}}};
        CHECK(bool(proveWorkgroupSafety(scratch, lanes)) == (writeMask == 3 && a != b));
      }
  for (const auto description : numericTypes) {
    const std::vector<WorkgroupScratch> typed{{description.type, 258}};
    proof = proveWorkgroupSafety(typed, stencil);
    CHECK(proof && proof->scratchBytes == 258 * (description.bits <= 32 ? 4u : 8u));
  }
  WorkgroupProofLimits limits;
  limits.accesses = 2;
  CHECK(proveWorkgroupSafety(scratch, stencil, limits).error().kind == Failure::Budget);
  limits = {};
  limits.lanes = 255;
  CHECK(proveWorkgroupSafety(scratch, stencil, limits).error().kind == Failure::Budget);
  limits = {};
  limits.phases = 1;
  CHECK(proveWorkgroupSafety(scratch, stencil, limits).error().kind == Failure::Budget);
  rejected({{NumericType::Float64, std::numeric_limits<std::uint64_t>::max()}}, stencil,
           Failure::Budget);
  rejected({{NumericType::Float32, 0}}, stencil, Failure::InvalidPlan);
  rejected({{static_cast<NumericType>(999), 1}}, stencil, Failure::InvalidPlan);
  rejected(scratch, {}, Failure::InvalidPlan);
  rejected(scratch, {{}}, Failure::InvalidPlan);
  WorkgroupLane point;
  point.phases.emplace_back();
  CHECK(proveWorkgroupSafety({}, {&point, 1}));
  std::puts("Workgroup ownership, initialization, collective traces and proof budgets passed");
}
