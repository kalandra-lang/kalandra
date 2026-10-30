#pragma once
#include "kalandra/Semantics.h"
#include <algorithm>

namespace kalandra {
inline std::vector<std::size_t> errorMembers(const TypedModule &module, std::size_t family) {
  const auto &members = module.records.at(family).errorMembers;
  return members.empty() ? std::vector<std::size_t>{family} : members;
}
inline std::optional<std::size_t> taskErrorFamily(const TypedModule &module) {
  for (std::size_t i = 0; i < module.records.size(); ++i)
    if (module.records[i].taskErrorFamily)
      return i;
  return {};
}
inline bool errorSubset(const TypedModule &module, std::optional<std::size_t> source,
                        std::optional<std::size_t> target) {
  if (source == target)
    return true;
  if (!source)
    source = taskErrorFamily(module);
  if (!target)
    target = taskErrorFamily(module);
  if (!source || !target)
    return false;
  if (*source >= module.records.size() || *target >= module.records.size())
    return false;
  const auto from = errorMembers(module, *source), to = errorMembers(module, *target);
  return std::ranges::all_of(from, [&](auto id) { return std::ranges::find(to, id) != to.end(); });
}
inline std::pair<std::size_t, std::size_t> errorOrigin(const TypedModule &module,
                                                       std::size_t family, std::size_t variant) {
  const auto &origins = module.records.at(family).errorOrigins;
  return origins.empty() ? std::pair{family, variant} : origins.at(variant);
}
} // namespace kalandra
