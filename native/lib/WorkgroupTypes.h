#pragma once
#include "kalandra/Semantics.h"

namespace kalandra {
// Canonical module identity survives import aliases and generic specialization.
// Wrappers do not erase capability identity when checking forbidden escapes.
inline std::optional<StandardRecord> workgroupCapability(const TypedModule &module,
                                                        const ValueType &type) {
  if (type.builtin != BuiltinValue::Record || !type.record || *type.record >= module.records.size())
    return {};
  const auto tag = module.records[*type.record].standard;
  if (tag == StandardRecord::WorkItem || tag == StandardRecord::Shared)
    return tag;
  return {};
}
inline bool plainWorkgroupCapability(const TypedModule &module, const ValueType &type,
                                     StandardRecord expected) {
  return workgroupCapability(module, type) == expected && !type.tensor && !type.array &&
         !type.task && !type.fallible && !type.device && !type.errorRecord;
}
} // namespace kalandra
