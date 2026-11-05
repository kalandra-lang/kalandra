#pragma once
#include "DeviceHeap.h"
#include <expected>
#include <set>

namespace kalandra {
struct DeviceCallSuspension {
  std::uint64_t returnPC;
  std::span<const unsigned> roots;
};

// A suspended frame traces its parent and only roots live at its return site.
// The active frame's source-PC roots are handled separately by the collector.
inline std::expected<DeviceTraceLayout, std::string>
planDeviceCallTrace(unsigned identity, unsigned words, unsigned parent, unsigned suspendedAt,
                    unsigned firstValue, std::span<const DeviceCallSuspension> sites,
                    std::size_t work) {
  if (identity <= 11 || firstValue > words || parent >= firstValue ||
      suspendedAt >= firstValue || parent == suspendedAt || !work)
    return std::unexpected("invalid GPU call trace layout");
  DeviceTraceLayout result{identity, words, {{parent, {}}}};
  --work;
  std::set<std::uint64_t> pcs;
  for (const auto &site : sites) {
    if (!work || site.roots.size() > (work - 1) / 2)
      return std::unexpected("GPU call trace layout exceeds work budget");
    work -= 1 + 2 * site.roots.size();
    if (!site.returnPC || !pcs.insert(site.returnPC).second)
      return std::unexpected("invalid or duplicate GPU call return PC");
    std::set<unsigned> offsets;
    for (const auto root : site.roots) {
      if (root < firstValue || root >= words || !offsets.insert(root).second)
        return std::unexpected("invalid or duplicate GPU suspended root offset");
      result.references.push_back({root, {{suspendedAt, site.returnPC}}});
    }
  }
  return result;
}
} // namespace kalandra
