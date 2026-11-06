#pragma once
#include "../lib/RecordStorage.h"
#include "kalandra/Semantics.h"
#include <algorithm>
#include <expected>
#include <map>
#include <set>
#include <span>

namespace kalandra {
// Canonical device cells, independent of the host's struct padding. Native
// pointers and collection descriptors are never copied into these cells.
struct DeviceAggregateLayout {
  std::size_t words = 0;
  std::vector<std::size_t> fields;
  std::vector<std::size_t> owned;
  // Zero means unconditional; enum payloads use their one-based variant tag.
  std::vector<std::size_t> fieldTags;
  std::vector<bool> boxed;
};
using DeviceAggregateLayouts = std::map<std::size_t, DeviceAggregateLayout>;
inline bool deviceScalarValue(const ValueType &type) {
  return !type.tensor && !type.array && !type.task && !type.fallible &&
         (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool);
}
inline bool deviceFlatArrayValue(const ValueType &type) {
  return type.array && type.tensor && type.shape.size() == 1 && !type.task && !type.fallible &&
         (type.builtin == BuiltinValue::Numeric || type.builtin == BuiltinValue::Bool);
}
inline bool deviceInlineRecordValue(const ValueType &type) {
  return type.record && type.builtin == BuiltinValue::Record && !type.tensor && !type.array &&
         !type.task && !type.fallible;
}
inline bool deviceRecordArrayValue(const ValueType &type) {
  return isRecordArray(type) && type.record && type.tensor && type.shape.size() == 1 &&
         !type.task && !type.fallible;
}
inline bool deviceCallableValue(const ValueType &type) {
  return type.builtin == BuiltinValue::Function && !type.array && !type.tensor &&
         !type.task && !type.fallible;
}
inline bool deviceTensorValue(const ValueType &type) {
  return type.tensor && !type.array && !type.task && !type.fallible &&
         type.builtin == BuiltinValue::Numeric && type.shape.size() <= 8;
}
// Frame planning can use nominal storage decisions to terminate expansion at
// arena handles. Flat boundary projections omit the storage plan.
inline std::expected<DeviceAggregateLayouts, std::string>
planDeviceAggregateLayouts(const TypedModule &input, std::span<const ValueType> values,
                           std::size_t budget = 300000,
                           const RecordStoragePlan *storage = nullptr, bool deviceClosures = false) {
  DeviceAggregateLayouts layouts;
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  const auto recordArray = [&](const ValueType &type) {
    return storage && deviceRecordArrayValue(type);
  };
  const auto closure = [&](const ValueType &type) {
    return deviceClosures && (deviceCallableValue(type) || deviceTensorValue(type));
  };
  std::vector<std::size_t> roots;
  std::set<std::size_t> scheduled;
  const auto schedule = [&](std::size_t id) {
    if (scheduled.insert(id).second)
      roots.push_back(id);
  };
  for (const auto &type : values) {
    if (deviceScalarValue(type) || deviceFlatArrayValue(type) || closure(type))
      continue;
    if (!deviceInlineRecordValue(type) && !recordArray(type))
      return reject(
          "GPU checkpointed values support numeric/Bool scalars, local numeric/Bool arrays and "
          "plain-data records/enums; owned aggregates and tensor loop state need device layouts");
    schedule(*type.record);
  }
  for (std::size_t root = 0; root < roots.size(); ++root) {
    std::vector<std::pair<std::size_t, std::size_t>> pending{{roots[root], 0}};
    std::set<std::size_t> active;
    while (!pending.empty()) {
      auto &[id, field] = pending.back();
      if (layouts.contains(id)) {
        pending.pop_back();
        continue;
      }
      if (id >= input.records.size())
        return reject("GPU aggregate layout requires a checked nominal identity");
      if (field == 0 && !active.insert(id).second)
        return reject("recursive owned device records require device storage");
      const auto &record = input.records[id];
      const auto boxed = [&](std::size_t index) {
        return storage && storage->fields.at(id).at(index) == RecordFieldStorage::ManagedReference;
      };
      if (field < record.fields.size()) {
        if (!budget)
          return reject("GPU aggregate layout exceeds configured work budget");
        --budget;
        const auto &child = record.fields[field].type;
        const bool indirect = boxed(field++) || recordArray(child);
        if (deviceScalarValue(child) || deviceFlatArrayValue(child) || closure(child))
          continue;
        if (indirect) {
          schedule(*child.record);
          continue;
        }
        if (!deviceInlineRecordValue(child))
          return reject("GPU plain-data records cannot contain owned or host-only fields");
        if (active.contains(*child.record))
          return reject("recursive owned device records require device storage");
        if (!layouts.contains(*child.record))
          pending.emplace_back(*child.record, 0);
        continue;
      }
      DeviceAggregateLayout layout;
      layout.fieldTags.resize(record.fields.size());
      if (record.variants.size() > budget)
        return reject("GPU aggregate variant layout exceeds configured work budget");
      budget -= record.variants.size();
      for (std::size_t variant = 0; variant < record.variants.size(); ++variant) {
        if (record.variants[variant].fields.size() > budget)
          return reject("GPU aggregate variant layout exceeds configured work budget");
        budget -= record.variants[variant].fields.size();
        for (auto slot : record.variants[variant].fields) {
          if (!slot || slot >= layout.fieldTags.size() || layout.fieldTags[slot])
            return reject("GPU aggregate layout requires checked enum payload fields");
          layout.fieldTags[slot] = variant + 1;
        }
      }
      for (std::size_t index = 0; index < record.fields.size(); ++index) {
        const auto &member = record.fields[index];
        layout.fields.push_back(layout.words);
        layout.boxed.push_back(boxed(index));
        const bool handle = deviceFlatArrayValue(member.type) || recordArray(member.type) || closure(member.type) ||
                            layout.boxed.back();
        const auto size = deviceScalarValue(member.type) || handle
                              ? 1
                              : layouts.at(*member.type.record).words;
        const auto owned = handle ? 1
                           : deviceInlineRecordValue(member.type)
                               ? layouts.at(*member.type.record).owned.size()
                               : 0;
        if (owned > budget)
          return reject("GPU aggregate owned layout exceeds configured work budget");
        budget -= owned;
        if (handle)
          layout.owned.push_back(layout.words);
        else if (deviceInlineRecordValue(member.type))
          for (auto offset : layouts.at(*member.type.record).owned)
            layout.owned.push_back(layout.words + offset);
        if (size > (UINT32_MAX - 4) / 2 - layout.words)
          return reject("GPU aggregate layout exceeds the device ABI");
        layout.words += size;
      }
      layout.words = std::max<std::size_t>(1, layout.words);
      layouts.emplace(id, std::move(layout));
      active.erase(id);
      pending.pop_back();
    }
  }
  return layouts;
}
} // namespace kalandra
