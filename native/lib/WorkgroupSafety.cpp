#include "kalandra/WorkgroupSafety.h"
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace kalandra {
std::expected<WorkgroupProof, WorkgroupProofError>
proveWorkgroupSafety(std::span<const WorkgroupScratch> scratch,
                     std::span<const WorkgroupLane> lanes, const WorkgroupProofLimits &limits) {
  const auto fail = [](WorkgroupProofFailure kind, std::string message, std::size_t lane = 0,
                       std::size_t phase = 0, std::size_t access = 0) {
    return std::unexpected(WorkgroupProofError{kind, lane, phase, access, std::move(message)});
  };
  using Failure = WorkgroupProofFailure;
  if (lanes.empty())
    return fail(Failure::InvalidPlan, "a workgroup proof requires participating lanes");
  if (lanes.size() > limits.lanes || scratch.size() > limits.scratchElements)
    return fail(Failure::Budget, "workgroup safety proof exceeds its lane or scratch budget");
  std::uint64_t elements = 0, bytes = 0;
  std::vector<std::vector<bool>> initialized;
  for (const auto &site : scratch) {
    const auto carrier = workgroupCarrierBytes(site.element);
    if (!carrier || !site.elements)
      return fail(Failure::InvalidPlan, "shared scratch requires a numeric type and positive size");
    if (site.elements > limits.scratchElements - elements ||
        site.elements > (std::numeric_limits<std::uint64_t>::max() - bytes) / carrier)
      return fail(Failure::Budget, "workgroup safety proof exceeds its scratch budget");
    elements += site.elements;
    bytes += site.elements * carrier;
    initialized.emplace_back(site.elements, false);
  }
  const auto phases = lanes.front().phases.size();
  if (!phases)
    return fail(Failure::InvalidPlan, "a workgroup proof requires a final phase");
  if (phases > limits.phases)
    return fail(Failure::Budget, "workgroup safety proof exceeds its collective budget");
  std::size_t accesses = 0;
  for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
    const auto &trace = lanes[lane].phases;
    if (trace.size() != phases)
      return fail(Failure::DivergentBarrier, "participating lanes have different barrier traces", lane);
    for (std::size_t phase = 0; phase < phases; ++phase) {
      if (trace[phase].barrier != lanes.front().phases[phase].barrier)
        return fail(Failure::DivergentBarrier, "participating lanes reach different barrier sites",
                    lane, phase);
      if (trace[phase].barrier.has_value() != (phase + 1 < phases))
        return fail(Failure::InvalidPlan, "only the final workgroup phase may omit its barrier",
                    lane, phase);
      if (trace[phase].accesses.size() > limits.accesses - accesses)
        return fail(Failure::Budget, "workgroup safety proof exceeds its access budget", lane, phase);
      accesses += trace[phase].accesses.size();
      for (std::size_t access = 0; access < trace[phase].accesses.size(); ++access) {
        const auto &item = trace[phase].accesses[access];
        if (item.scratch >= scratch.size() || item.index >= scratch[item.scratch].elements)
          return fail(Failure::Bounds, "shared scratch index is outside its allocation", lane,
                      phase, access);
      }
    }
  }
  using Address = std::pair<std::size_t, std::uint64_t>;
  for (std::size_t phase = 0; phase < phases; ++phase) {
    struct Accessors {
      std::optional<std::size_t> writer, reader;
      bool multipleReaders = false;
    };
    std::map<Address, Accessors> owners;
    std::set<Address> committed;
    for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
      std::set<Address> local;
      const auto &items = lanes[lane].phases[phase].accesses;
      for (std::size_t access = 0; access < items.size(); ++access) {
        const auto &item = items[access];
        const Address address{item.scratch, item.index};
        auto &owner = owners[address];
        if (item.write) {
          if (owner.writer && *owner.writer != lane)
            return fail(Failure::WriteRace, "shared scratch has conflicting writes between barriers",
                        lane, phase, access);
          if (owner.reader && (*owner.reader != lane || owner.multipleReaders))
            return fail(Failure::ReadRace, "shared scratch read and write require a barrier",
                        lane, phase, access);
          owner.writer = lane;
          if (item.definite) {
            local.insert(address);
            committed.insert(address);
          }
        } else {
          if (owner.writer && *owner.writer != lane)
            return fail(Failure::ReadRace, "shared scratch read and write require a barrier",
                        lane, phase, access);
          if (!initialized[item.scratch][item.index] && !local.contains(address))
            return fail(Failure::UninitializedRead, "shared scratch may be read before initialization",
                        lane, phase, access);
          if (owner.reader && *owner.reader != lane)
            owner.multipleReaders = true;
          else if (!owner.reader)
            owner.reader = lane;
        }
      }
    }
    for (const auto &[site, index] : committed)
      initialized[site][index] = true;
  }
  return WorkgroupProof{lanes.size(), phases, accesses, bytes};
}
} // namespace kalandra
