#pragma once
#include "../runtime/DeviceGraphWire.h"
#include "../runtime/NumericStorage.h"
#include "AggregateNativeLayout.h"
#include "AggregateProjection.h"
#include <tuple>

namespace kalandra {
struct AggregateGraphSchema {
  struct Cell {
    runtime::DeviceGraphCell rule{};
    std::vector<runtime::DeviceGraphGuard> guards;
    std::uint64_t nativeOffset = 0, nativeBytes = 0;
  };
  struct Layout {
    ValueType type;
    std::vector<Cell> cells;
    std::uint64_t nativeStride = 0, nativeAlignment = 0;
  };
  std::vector<Layout> layouts;
  std::vector<std::size_t> roots;
  std::optional<std::size_t> captureJoin;
};

// Append a transport-only layout to the shared graph schema. Root order is
// argument order, including repeated nominal types and nullable arrays.
inline bool appendAggregateCaptureJoin(AggregateGraphSchema &schema, std::size_t &budget) {
  if (schema.captureJoin || schema.roots.size() < 2 ||
      std::ranges::any_of(schema.roots,
                          [&](auto root) { return !root || root > schema.layouts.size(); }) ||
      !consumeAggregateProjectionWork(budget, schema.roots.size()))
    return false;
  AggregateGraphSchema::Layout join;
  join.nativeStride = schema.roots.size();
  join.nativeAlignment = 1;
  join.cells.reserve(schema.roots.size());
  for (std::size_t i = 0; i < schema.roots.size(); ++i) {
    const auto target = schema.roots[i];
    join.cells.push_back({{0, target, schema.layouts[target - 1].type.array}, {}, i, 1});
  }
  schema.layouts.push_back(std::move(join));
  schema.captureJoin = schema.layouts.size();
  return true;
}

inline std::expected<AggregateGraphSchema, std::string>
planAggregateGraphSchema(const TypedModule &input, std::span<const ValueType> roots,
                         std::span<const NativeAggregateABI> native, std::size_t budget = 300000) {
  const auto reject = [](std::string error) { return std::unexpected(std::move(error)); };
  if (native.size() != input.records.size())
    return reject("GPU graph schema requires target-native record layouts");
  auto storage = planRecordStorage(input, budget);
  if (!storage)
    return reject(storage.error().message);
  auto canonical = planDeviceAggregateLayouts(input, roots, budget, &*storage, true);
  if (!canonical)
    return reject(canonical.error());

  AggregateGraphSchema plan;
  using Key = std::tuple<BuiltinValue, NumericType, std::optional<std::size_t>, bool,
                         std::optional<std::size_t>>;
  std::map<Key, std::size_t> ids;
  const auto schedule = [&](const ValueType &type) {
    const Key key{type.builtin,
                  type.builtin == BuiltinValue::Numeric ? type.element : NumericType::UInt8,
                  type.record, type.array,
                  deviceTensorValue(type) ? std::optional{type.shape.size()} : std::nullopt};
    auto [found, inserted] = ids.try_emplace(key, plan.layouts.size() + 1);
    if (inserted)
      plan.layouts.push_back({type, {}});
    return found->second;
  };
  const auto scalarRule = [](const ValueType &type) {
    if (isBooleanStorage(type))
      return runtime::DeviceGraphCell{1};
    const auto bits = describeNumeric(type.element)->bits;
    return runtime::DeviceGraphCell{bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1};
  };

  for (const auto &root : roots) {
    if (!consumeAggregateProjectionWork(budget, 1))
      return reject("GPU graph schema exceeds configured work budget");
    plan.roots.push_back(schedule(root));
  }
  for (std::size_t layout = 0; layout < plan.layouts.size(); ++layout) {
    const auto type = plan.layouts[layout].type;
    if (deviceTensorValue(type)) {
      if (!consumeAggregateProjectionWork(budget, 1))
        return reject("GPU graph schema exceeds configured work budget");
      auto &tensor = plan.layouts[layout];
      tensor.nativeStride = tensor.nativeAlignment = sizeof(std::uint64_t);
      tensor.cells.push_back({{UINT64_MAX}, {}, 0, sizeof(std::uint64_t)});
      continue;
    }
    if (deviceScalarValue(type) || deviceFlatArrayValue(type)) {
      if (!consumeAggregateProjectionWork(budget, 1))
        return reject("GPU graph schema exceeds configured work budget");
      const auto bytes = isBooleanStorage(type) ? 1 : describeNumeric(type.element)->bits / 8;
      plan.layouts[layout].nativeStride = bytes;
      plan.layouts[layout].nativeAlignment =
          isBooleanStorage(type) ? 1 : *runtime::numericAlignment(type.element);
      plan.layouts[layout].cells.push_back(
          {scalarRule(type), {}, 0, static_cast<std::uint64_t>(bytes)});
      continue;
    }
    const auto element = type.array ? recordArrayElement(type) : type;
    if (!element.record)
      return reject("GPU result graph requires numeric, Bool or nominal storage");
    const auto stride = canonical->at(*element.record).words;
    if (!consumeAggregateProjectionWork(budget, stride))
      return reject("GPU graph schema exceeds configured work budget");
    auto projection =
        projectDeviceAggregateOccurrences(input, std::span(&element, 1), *canonical, budget);
    if (!projection)
      return reject(projection.error());
    std::vector<AggregateGraphSchema::Cell> cells(stride);
    std::vector<std::uint64_t> nativeOffsets(projection->nodes.size());
    const auto nativeStride = native[*element.record].nativeBytes;
    plan.layouts[layout].nativeStride = nativeStride;
    plan.layouts[layout].nativeAlignment = native[*element.record].nativeAlignment;
    for (std::size_t i = 1; i < projection->nodes.size(); ++i) {
      const auto &node = projection->nodes[i];
      const auto &parent = projection->nodes[node.parent];
      const auto &fields = native[*parent.type.record].fields;
      if (node.field >= fields.size())
        return reject("GPU graph field is missing from target-native layout");
      const auto &field = fields[node.field];
      const auto base = nativeOffsets[node.parent];
      if (field.nativeOffset > nativeStride - base ||
          field.nativeBytes > nativeStride - base - field.nativeOffset)
        return reject("GPU graph field exceeds target-native element extent");
      nativeOffsets[i] = base + field.nativeOffset;
    }
    for (const auto leaf : projection->leaves) {
      const auto &node = projection->nodes[leaf];
      auto &cell = cells.at(node.deviceWord);
      const auto &parent = projection->nodes[node.parent];
      cell.nativeOffset = nativeOffsets[leaf];
      cell.nativeBytes = native[*parent.type.record].fields[node.field].nativeBytes;
      if (deviceCallableValue(node.type))
        return reject("GPU result graph cannot export callable values");
      if (node.boxed || node.type.array || deviceTensorValue(node.type)) {
        cell.rule.target = schedule(node.type);
        cell.rule.nullable = node.type.array;
      } else {
        cell.rule = scalarRule(node.type);
        if (node.discriminator) {
          cell.rule.minimum = 1;
          cell.rule.maximum = input.records.at(*parent.type.record).variants.size();
        }
      }
      for (auto child = leaf; projection->nodes[child].parent != DeviceAggregateOccurrence::none;
           child = projection->nodes[child].parent) {
        if (!consumeAggregateProjectionWork(budget, 1))
          return reject("GPU graph guard projection exceeds configured work budget");
        const auto &member = projection->nodes[child];
        if (member.activeTag)
          cell.guards.push_back({projection->nodes[member.parent].deviceWord, member.activeTag});
      }
      std::ranges::reverse(cell.guards);
    }
    plan.layouts[layout].cells = std::move(cells);
  }
  return plan;
}
} // namespace kalandra
