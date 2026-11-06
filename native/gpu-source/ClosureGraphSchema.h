#pragma once
#include "../metal-source/DeviceClosureImport.h"
#include "AggregateGraphSchema.h"
#include "ClosureNativeLayout.h"

namespace kalandra {
struct ClosureGraphSchema {
  enum class Kind { Value, Callable, Environment };
  AggregateGraphSchema graph;
  std::vector<Kind> kinds;
  std::vector<std::optional<FunctionId>> environments;
  std::vector<std::vector<FunctionId>> targets;
  std::vector<unsigned> arenaLayouts;
  std::vector<DeviceClosureImport> imports;
};

// One shared directory for ordinary values, callable carriers and environments.
// Native offsets remain those of the original source ABI; only wire cells use
// target tags and references. All targets come from the admitted entry plan.
inline std::expected<ClosureGraphSchema, std::string>
planClosureGraphSchema(const TypedModule &input, std::span<const ValueType> roots,
                       const ClosureTransferPlan &closures, const NativeClosureABI &closureABI,
                       std::span<const NativeAggregateABI> native, std::size_t budget = 300000) {
  const auto reject = [](std::string message) { return std::unexpected(std::move(message)); };
  if (native.size() != input.records.size() ||
      closures.environments.size() != closureABI.environments.size() || !closureABI.carrierBytes ||
      !closureABI.carrierAlignment || closureABI.environmentOffset >= closureABI.carrierBytes)
    return reject("closure graph requires matching checked native schemas");
  const auto consume = [&](std::size_t count) {
    return consumeAggregateProjectionWork(budget, count);
  };
  if (roots.empty() || !consume(roots.size()))
    return reject("closure graph roots exceed configured work budget");
  std::vector<ValueType> allTypes(roots.begin(), roots.end());
  std::map<FunctionId, const NativeClosureLayout *> environments;
  std::map<FunctionId, ValueType> signatures;
  std::set<FunctionId> admitted;
  for (const auto &environment : closures.environments)
    admitted.insert(environment.target);
  // FunctionReference carries the checked public signature. A lambda body may
  // have more precise tensor result extents than that declared signature.
  for (const auto &expression : input.expressions) {
    if (!consume(1))
      return reject("closure graph reference discovery exceeds its work budget");
    const auto *reference = std::get_if<FunctionReference>(&expression.operation);
    if (!reference || !admitted.contains(reference->function))
      continue;
    if (!deviceCallableValue(expression.type) || !expression.type.callable)
      return reject("closure graph requires a checked callable reference signature");
    const auto [found, inserted] = signatures.emplace(reference->function, expression.type);
    if (!inserted && !sameFunctionValueType(found->second, expression.type))
      return reject("closure graph target has inconsistent checked reference signatures");
  }
  for (std::size_t i = 0; i < closures.environments.size(); ++i) {
    const auto &environment = closures.environments[i];
    const auto &abi = closureABI.environments[i];
    if (!consume(1 + environment.captures.size()) || environment.target >= input.functions.size() ||
        abi.target != environment.target || abi.captures.size() != environment.captures.size() ||
        !environments.emplace(environment.target, &abi).second)
      return reject("closure graph has invalid target metadata or exceeds its work budget");
    const auto &fn = input.functions[environment.target];
    if (fn.captureCount > fn.parameters.size() || fn.captureCount != environment.captures.size())
      return reject("closure graph capture arity mismatch");
    if (!signatures.contains(environment.target))
      return reject("closure graph target has no checked reference signature");
    for (std::size_t c = 0; c < environment.captures.size(); ++c) {
      const auto symbol = environment.captures[c];
      if (symbol >= input.symbols.size() || abi.captures[c].symbol != symbol ||
          symbol != fn.parameters[fn.parameters.size() - fn.captureCount + c])
        return reject("closure graph capture identity mismatch");
      allTypes.push_back(input.symbols[symbol].type);
    }
  }
  auto storage = planRecordStorage(input, budget);
  if (!storage)
    return reject(storage.error().message);
  auto canonical = planDeviceAggregateLayouts(input, allTypes, budget, &*storage, true);
  if (!canonical)
    return reject(canonical.error());
  ClosureGraphSchema out;
  std::map<FunctionId, std::size_t> environmentIds;
  std::size_t nextIdentity = 12 + 2 * input.records.size() + 2 * input.functions.size();
  const auto schedule = [&](const ValueType &type) -> std::size_t {
    auto canonicalType = type;
    if (canonicalType.array)
      canonicalType.shape = {{std::nullopt, 1}};
    else if (deviceTensorValue(canonicalType))
      for (auto &extent : canonicalType.shape)
        extent = {std::nullopt, 1};
    for (std::size_t i = 0; i < out.graph.layouts.size(); ++i) {
      if (!consume(1))
        return 0;
      if (out.kinds[i] != ClosureGraphSchema::Kind::Environment &&
          sameFunctionValueType(out.graph.layouts[i].type, canonicalType))
        return i + 1;
    }
    if (!consume(1) || nextIdentity >= UINT32_MAX)
      return 0;
    out.graph.layouts.push_back({canonicalType, {}});
    out.kinds.push_back(deviceCallableValue(type) ? ClosureGraphSchema::Kind::Callable
                                                  : ClosureGraphSchema::Kind::Value);
    out.environments.emplace_back();
    out.targets.emplace_back();
    out.arenaLayouts.push_back(
        deviceCallableValue(type) ? nextIdentity++
        : deviceTensorValue(type)
            ? runtime::DeviceHeapBlockABI::tensorLayout(type.element, type.shape.size())
        : type.record ? runtime::DeviceHeapBlockABI::nominalLayout(*type.record, type.array)
                      : runtime::DeviceArrayWireABI::layout(type.element,
                                                            type.builtin == BuiltinValue::Bool));
    return out.graph.layouts.size();
  };
  for (const auto &root : roots) {
    auto id = schedule(root);
    if (!id)
      return reject("closure graph roots exceed configured work budget");
    out.graph.roots.push_back(id);
  }
  const auto scalarRule = [](const ValueType &type) {
    if (type.builtin == BuiltinValue::Bool)
      return runtime::DeviceGraphCell{1};
    const auto bits = describeNumeric(type.element)->bits;
    return runtime::DeviceGraphCell{bits == 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1};
  };
  const auto append = [&](const ValueType &type, std::uint64_t offset, std::uint64_t bytes,
                          std::vector<AggregateGraphSchema::Cell> &cells) -> bool {
    if (!consume(1))
      return false;
    if (!deviceInlineRecordValue(type)) {
      AggregateGraphSchema::Cell cell{{}, {}, offset, bytes};
      if (deviceScalarValue(type))
        cell.rule = scalarRule(type);
      else if (deviceCallableValue(type) || deviceFlatArrayValue(type) ||
               deviceRecordArrayValue(type) || deviceTensorValue(type)) {
        cell.rule.target = schedule(type);
        cell.rule.nullable = type.array;
        if (!cell.rule.target)
          return false;
      } else
        return false;
      cells.push_back(std::move(cell));
      return true;
    }
    auto projection =
        projectDeviceAggregateOccurrences(input, std::span(&type, 1), *canonical, budget);
    if (!projection || !consume(canonical->at(*type.record).words))
      return false;
    const auto first = cells.size();
    cells.resize(first + canonical->at(*type.record).words);
    std::vector<std::uint64_t> offsets(projection->nodes.size(), offset);
    for (std::size_t i = 1; i < projection->nodes.size(); ++i) {
      const auto &node = projection->nodes[i];
      const auto &parent = projection->nodes[node.parent];
      if (node.field >= native[*parent.type.record].fields.size())
        return false;
      const auto &field = native[*parent.type.record].fields.at(node.field);
      if (field.nativeOffset > UINT64_MAX - offsets[node.parent])
        return false;
      offsets[i] = offsets[node.parent] + field.nativeOffset;
    }
    for (auto leaf : projection->leaves) {
      const auto &node = projection->nodes[leaf];
      const auto &parent = projection->nodes[node.parent];
      const auto &field = native[*parent.type.record].fields.at(node.field);
      if (offsets[leaf] < offset || offsets[leaf] - offset > bytes ||
          field.nativeBytes > bytes - (offsets[leaf] - offset))
        return false;
      auto &cell = cells[first + node.deviceWord];
      cell.nativeOffset = offsets[leaf];
      cell.nativeBytes = field.nativeBytes;
      if (node.boxed || node.type.array || deviceCallableValue(node.type) || deviceTensorValue(node.type)) {
        cell.rule.target = schedule(node.type);
        cell.rule.nullable = node.type.array;
        if (!cell.rule.target)
          return false;
      } else {
        cell.rule = scalarRule(node.type);
        if (node.discriminator) {
          cell.rule.minimum = 1;
          cell.rule.maximum = input.records[*parent.type.record].variants.size();
        }
      }
      for (auto child = leaf; projection->nodes[child].parent != DeviceAggregateOccurrence::none;
           child = projection->nodes[child].parent) {
        if (!consume(1))
          return false;
        const auto &member = projection->nodes[child];
        if (member.activeTag)
          cell.guards.push_back(
              {first + projection->nodes[member.parent].deviceWord, member.activeTag});
      }
      std::ranges::reverse(cell.guards);
    }
    return true;
  };
  for (std::size_t i = 0; i < out.graph.layouts.size(); ++i) {
    const auto type = out.graph.layouts[i].type;
    std::vector<AggregateGraphSchema::Cell> cells;
    std::uint64_t stride = 0, alignment = 0;
    if (out.kinds[i] == ClosureGraphSchema::Kind::Callable) {
      stride = closureABI.carrierBytes;
      alignment = closureABI.carrierAlignment;
      cells.push_back({{UINT64_MAX, 0, false, 1}, {}, 0, stride}); // Projected target tag.
      for (const auto &[target, signature] : signatures) {
        if (!consume(1))
          return reject("closure graph target discovery exceeds its work budget");
        if (!sameFunctionValueType(type, signature))
          continue;
        out.targets[i].push_back(target);
        const auto &env = *environments.at(target);
        DeviceClosureImport import{target, out.arenaLayouts[i], 0, 0, 0};
        if (!env.captures.empty()) {
          auto found = environmentIds.find(target);
          if (found == environmentIds.end()) {
            if (!consume(1) || nextIdentity >= UINT32_MAX)
              return reject("closure graph environment directory exceeds its work budget");
            const auto id = out.graph.layouts.size() + 1;
            environmentIds.emplace(target, id);
            out.graph.layouts.push_back({{}, {}});
            out.kinds.push_back(ClosureGraphSchema::Kind::Environment);
            out.environments.push_back(target);
            out.targets.emplace_back();
            out.arenaLayouts.push_back(nextIdentity++);
            found = environmentIds.find(target);
          }
          import.environmentLayout = out.arenaLayouts[found->second - 1];
          import.environmentWord = cells.size();
          cells.push_back({{0, found->second, false},
                           {{0, target + 1}},
                           closureABI.environmentOffset,
                           closureABI.carrierBytes - closureABI.environmentOffset});
        }
        out.imports.push_back(import);
      }
      if (out.targets[i].empty() || cells.size() > 256)
        return reject("closure graph callable requires a bounded nonempty admitted target set");
      for (auto &import : out.imports)
        if (import.carrierLayout == out.arenaLayouts[i])
          import.carrierWords = cells.size();
    } else if (out.kinds[i] == ClosureGraphSchema::Kind::Environment) {
      const auto &env = *environments.at(*out.environments[i]);
      stride = env.bytes;
      alignment = env.alignment;
      for (const auto &capture : env.captures)
        if (capture.offset > stride || capture.bytes > stride - capture.offset ||
            !append(input.symbols[capture.symbol].type, capture.offset, capture.bytes, cells))
          return reject("closure graph capture has unsupported storage or exceeds its work budget");
    } else if (deviceTensorValue(type)) {
      // The resolver validates the native descriptor and converts extents and
      // element bits to canonical UInt64 words; it never transports pointers.
      stride = alignment = 8;
      cells.push_back({{UINT64_MAX}, {}, 0, 8});
    } else {
      const auto element = type.array ? recordArrayElement(type) : type;
      if (element.record) {
        stride = native[*element.record].nativeBytes;
        alignment = native[*element.record].nativeAlignment;
      } else {
        stride =
            element.builtin == BuiltinValue::Bool ? 1 : describeNumeric(element.element)->bytes();
        alignment =
            element.builtin == BuiltinValue::Bool ? 1 : *runtime::numericAlignment(element.element);
      }
      if (!append(element, 0, stride, cells))
        return reject("closure graph value has unsupported storage or exceeds its work budget");
    }
    out.graph.layouts[i].nativeStride = stride;
    out.graph.layouts[i].nativeAlignment = alignment;
    out.graph.layouts[i].cells = std::move(cells);
  }
  return out;
}
} // namespace kalandra
