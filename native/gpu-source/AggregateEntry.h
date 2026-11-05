#pragma once
#include "AggregateLayout.h"
#include <limits>

namespace kalandra {
// A node is an occurrence, not a nominal type: two fields of the same type
// remain distinct captures. Parents precede children, so consumers can compute
// native offsets and activity without recursive traversal or copied field paths.
struct DeviceEntryNode {
  static constexpr auto none = std::numeric_limits<std::size_t>::max();
  ValueType type;
  std::size_t parameter, parent = none, field = none, activeTag = 0;
  std::vector<std::size_t> children;
  bool discriminator = false;
};
struct DeviceEntryPlan {
  std::vector<DeviceEntryNode> nodes;
  std::vector<std::size_t> roots, leaves;
};
struct NormalizedDeviceEntry {
  TypedModule module;
  FunctionId function;
  DeviceEntryPlan plan;
};
// Flatten only the entry ABI. The original helper, including its arithmetic and
// control flow, is called by the generated device wrapper. No source computation
// moves to the host. Native ownership and guarded field extraction are separate
// consumers of this plan; this function never interprets native bytes.
inline std::expected<NormalizedDeviceEntry, std::string>
normalizeDeviceAggregateEntry(const TypedModule &input, FunctionId function,
                              std::size_t budget = 300000) {
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  if (function >= input.functions.size())
    return reject("GPU aggregate entry requires a checked function identity");
  const auto &original = input.functions[function];
  if (!deviceScalarValue(original.resultType) && !deviceFlatArrayValue(original.resultType))
    return reject("GPU aggregate results require owned graph transport");
  std::vector<ValueType> types;
  for (auto parameter : original.parameters) {
    if (parameter >= input.symbols.size())
      return reject("GPU aggregate entry requires checked parameter identities");
    if (input.symbols[parameter].mutableAccess)
      return reject("GPU captured aggregate inputs are read-only");
    types.push_back(input.symbols[parameter].type);
  }
  auto layouts = planDeviceAggregateLayouts(input, types, budget);
  if (!layouts)
    return reject(layouts.error());
  NormalizedDeviceEntry result{input, input.functions.size(), {}};
  auto &plan = result.plan;
  const auto consume = [&](std::size_t amount) {
    if (amount > budget)
      return false;
    budget -= amount;
    return true;
  };
  for (std::size_t parameter = 0; parameter < types.size(); ++parameter) {
    if (!consume(1))
      return reject("GPU aggregate entry exceeds configured work budget");
    plan.roots.push_back(plan.nodes.size());
    plan.nodes.push_back({types[parameter], parameter, DeviceEntryNode::none,
                            DeviceEntryNode::none, 0, {}, false});
  }
  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    const auto type = plan.nodes[i].type;
    if (!deviceInlineRecordValue(type)) {
      plan.leaves.push_back(i);
      continue;
    }
    const auto &record = input.records.at(*type.record);
    const auto &layout = layouts->at(*type.record);
    if (!consume(record.fields.size()))
      return reject("GPU aggregate entry exceeds configured work budget");
    if (!record.variants.empty() &&
        (record.fields.empty() || record.fields[0].type.builtin != BuiltinValue::Numeric ||
         record.fields[0].type.element != NumericType::Int64 ||
         !deviceScalarValue(record.fields[0].type)))
      return reject("GPU enum entry requires a checked native discriminator");
    for (std::size_t field = 0; field < record.fields.size(); ++field) {
      const auto child = plan.nodes.size();
      const auto parameter = plan.nodes[i].parameter;
      plan.nodes[i].children.push_back(child);
      plan.nodes.push_back({record.fields[field].type, parameter, i, field,
                              layout.fieldTags[field], {},
                              !record.variants.empty() && field == 0});
    }
  }
  TypedFunction wrapper;
  wrapper.name = "$gpu.aggregate.entry." + std::to_string(function);
  wrapper.declaration = original.declaration;
  wrapper.resultType = original.resultType;
  wrapper.declaredResult = original.declaredResult;
  wrapper.preconditions = original.preconditions;
  std::vector<ValueId> values(plan.nodes.size());
  const auto add = [&](const ValueType &type, auto operation) {
    const auto id = result.module.expressions.size();
    result.module.expressions.push_back({original.declaration, type, std::move(operation)});
    wrapper.expressions.push_back(id);
    return id;
  };
  for (auto node : plan.leaves) {
    if (!consume(2))
      return reject("GPU aggregate entry exceeds configured work budget");
    const auto symbol = result.module.symbols.size();
    result.module.symbols.push_back({"$capture." + std::to_string(node), plan.nodes[node].type,
                                     original.declaration, result.function, true});
    wrapper.parameters.push_back(symbol);
    values[node] = add(plan.nodes[node].type, SymbolRead{symbol});
  }
  ValueType boolean;
  boolean.builtin = BuiltinValue::Bool;
  for (std::size_t i = plan.nodes.size(); i-- > 0;) {
    const auto &node = plan.nodes[i];
    if (!deviceInlineRecordValue(node.type))
      continue;
    const auto &record = input.records.at(*node.type.record);
    if (record.variants.empty()) {
      if (!consume(1))
        return reject("GPU aggregate entry exceeds configured work budget");
      RecordValue value;
      for (std::size_t field = 0; field < node.children.size(); ++field)
        value.fields.push_back({field, values[node.children[field]]});
      values[i] = add(node.type, std::move(value));
      continue;
    }
    std::vector<ValueId> alternatives;
    if (record.variants.size() > budget / 4 || !consume(record.variants.size() * 4))
      return reject("GPU aggregate entry exceeds configured work budget");
    for (std::size_t variant = 0; variant < record.variants.size(); ++variant) {
      RecordValue value;
      value.variant = variant;
      for (auto field : record.variants[variant].fields)
        value.fields.push_back({field, values[node.children.at(field)]});
      alternatives.push_back(add(node.type, std::move(value)));
    }
    auto selected = alternatives.back();
    for (std::size_t variant = alternatives.size() - 1; variant-- > 0;) {
      const auto tag = add(record.fields[0].type, Int64Literal{static_cast<std::int64_t>(variant + 1)});
      const auto condition = add(boolean, ScalarCompare{ComparisonKind::Equal,
                                                         values[node.children[0]], tag});
      selected = add(node.type,
                     ConditionalOperation{condition,
                                          {{original.declaration, {}, alternatives[variant]}},
                                          {{original.declaration, {}, selected}},
                                          alternatives[variant], selected});
    }
    values[i] = selected;
  }
  if (!consume(1))
    return reject("GPU aggregate entry exceeds configured work budget");
  std::vector<ValueId> arguments;
  for (auto root : plan.roots)
    arguments.push_back(values[root]);
  wrapper.result = add(original.resultType, FunctionCall{function, std::move(arguments)});
  wrapper.body = {{original.declaration, {}, wrapper.result}};
  result.module.functions.push_back(std::move(wrapper));
  return result;
}
} // namespace kalandra
