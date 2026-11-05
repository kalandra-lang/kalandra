#pragma once
#include "AggregateLayout.h"
#include <limits>

namespace kalandra {
// Two fields of the same nominal type remain distinct occurrences.
// Parents precede children, so consumers can compute
// native offsets and activity without recursive traversal or copied field paths.
struct DeviceAggregateOccurrence {
  static constexpr auto none = std::numeric_limits<std::size_t>::max();
  ValueType type;
  std::size_t root, parent = none, field = none, activeTag = 0, deviceWord = 0;
  std::vector<std::size_t> children;
  bool discriminator = false;
};
struct DeviceAggregateProjection {
  std::vector<DeviceAggregateOccurrence> nodes;
  std::vector<std::size_t> roots, leaves;
};
inline bool consumeAggregateProjectionWork(std::size_t &budget, std::size_t amount) {
  if (amount > budget)
    return false;
  budget -= amount;
  return true;
}

// Shared capture/result schema. Device words are relative to their root,
// while each native consumer computes its own target-specific byte offsets.
inline std::expected<DeviceAggregateProjection, std::string>
projectDeviceAggregates(const TypedModule &input, std::span<const ValueType> types,
                        std::size_t &budget) {
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  auto layouts = planDeviceAggregateLayouts(input, types, budget);
  if (!layouts)
    return reject(layouts.error());
  DeviceAggregateProjection plan;
  for (std::size_t root = 0; root < types.size(); ++root) {
    if (!consumeAggregateProjectionWork(budget, 1))
      return reject("GPU aggregate projection exceeds configured work budget");
    plan.roots.push_back(plan.nodes.size());
    plan.nodes.push_back({types[root], root, DeviceAggregateOccurrence::none,
                          DeviceAggregateOccurrence::none, 0, 0, {}, false});
  }
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const auto type = plan.nodes[i].type;
    if (!deviceInlineRecordValue(type)) {
      plan.leaves.push_back(i);
      continue;
    }
    const auto &record = input.records.at(*type.record);
    const auto &layout = layouts->at(*type.record);
    if (!consumeAggregateProjectionWork(budget, record.fields.size()))
      return reject("GPU aggregate projection exceeds configured work budget");
    if (!record.variants.empty() &&
        (record.fields.empty() || record.fields[0].type.builtin != BuiltinValue::Numeric ||
         record.fields[0].type.element != NumericType::Int64 ||
         !deviceScalarValue(record.fields[0].type)))
      return reject("GPU enum projection requires a checked native discriminator");
    for (std::size_t field = 0; field < record.fields.size(); ++field) {
      const auto child = plan.nodes.size();
      const auto root = plan.nodes[i].root;
      plan.nodes[i].children.push_back(child);
      plan.nodes.push_back({record.fields[field].type, root, i, field, layout.fieldTags[field],
                            plan.nodes[i].deviceWord + layout.fields[field], {},
                            !record.variants.empty() && field == 0});
    }
  }
  return plan;
}
} // namespace kalandra
