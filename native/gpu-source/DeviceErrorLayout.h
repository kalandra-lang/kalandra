#pragma once

#include "../lib/ErrorFamilies.h"
#include "AggregateLayout.h"

namespace kalandra {
// Validate the same layouts that device frames use for ordinary owned values.
// A task's scheduling channel is produced by the host, outside those frames.
inline std::expected<DeviceAggregateLayouts, std::string>
planDeviceErrorPayloadLayouts(const TypedModule &input, std::span<const std::size_t> families,
                              std::size_t budget = 300000) {
  std::vector<ValueType> roots;
  if (families.size() > budget)
    return std::unexpected("GPU error layout exceeds configured work budget");
  roots.reserve(families.size());
  for (auto family : families) {
    if (family >= input.records.size() || !input.records[family].error)
      return std::unexpected("GPU error payload requires a checked application-error family");
    for (auto member : errorMembers(input, family)) {
      if (!budget)
        return std::unexpected("GPU error layout exceeds configured work budget");
      --budget;
      if (member >= input.records.size() || !input.records[member].error)
        return std::unexpected("GPU error payload requires a checked application-error family");
      if (input.records[member].taskErrorFamily)
        return std::unexpected("GPU functions cannot declare the host TaskError scheduling channel");
    }
    ValueType root;
    root.builtin = BuiltinValue::Record;
    root.record = family;
    roots.push_back(std::move(root));
  }
  auto storage = planRecordStorage(input, budget);
  if (!storage)
    return std::unexpected(storage.error().message);
  return planDeviceAggregateLayouts(input, roots, budget, &*storage, true);
}
} // namespace kalandra
