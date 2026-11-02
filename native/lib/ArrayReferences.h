#pragma once
#include "kalandra/Semantics.h"
#include <type_traits>
#include <unordered_map>
#include <vector>
namespace kalandra {
// This proof controls private host-buffer import only. It grants no source write
// permission. A fresh functional result cannot expose its caller's input backing.
// Unknown/cyclic flows remain borrowed. Capture, aggregate and indirect-call
// descriptors are checked as roots as well as ordinary function results.
inline std::vector<bool> managedNumericArrayInputFunctions(const TypedModule &module) {
  // Reassignable array bindings will require flow-sensitive SSA origins. Do
  // not infer freshness from their last written value when they are admitted.
  for (const auto &symbol : module.symbols)
    if (symbol.reassignable && symbol.type.array && symbol.type.builtin == BuiltinValue::Numeric)
      return std::vector<bool>(module.functions.size(), true);
  std::unordered_map<SymbolId, ValueId> bindings;
  const auto statements = [&](const auto &body) {
    for (const auto &statement : body)
      if (statement.binding)
        bindings[*statement.binding] = statement.value;
  };
  for (const auto &function : module.functions)
    statements(function.body);
  for (const auto &expression : module.expressions)
    std::visit(
        [&](const auto &operation) {
          if constexpr (requires { operation.body; })
            statements(operation.body);
          if constexpr (requires {
                          operation.whenTrue;
                          operation.whenFalse;
                        }) {
            statements(operation.whenTrue);
            statements(operation.whenFalse);
          }
          if constexpr (requires { operation.cases; })
            for (const auto &arm : operation.cases)
              statements(arm.body);
        },
        expression.operation);
  std::vector<unsigned char> states(module.expressions.size());
  std::vector<bool> fresh(module.expressions.size());
  struct Frame {
    ValueId value;
    bool finish = false;
  };
  const auto isFresh = [&](ValueId root) {
    std::vector<Frame> pending{{root}};
    while (!pending.empty()) {
      const auto [id, finish] = pending.back();
      pending.pop_back();
      if (id >= module.expressions.size())
        return false;
      if (states[id] == 2)
        continue;
      std::vector<ValueId> dependencies;
      bool known = false;
      std::visit(
          [&](const auto &operation) {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<T, ArrayMap> || std::is_same_v<T, ArrayConcat> ||
                          std::is_same_v<T, ArraySlice> || std::is_same_v<T, TensorToArray> ||
                          std::is_same_v<T, TensorLiteral>)
              known = true;
            else if constexpr (std::is_same_v<T, CollectionQuery>)
              known = operation.kind == CollectionQueryKind::Shape;
            else if constexpr (std::is_same_v<T, SymbolRead>) {
              if (auto binding = bindings.find(operation.symbol); binding != bindings.end()) {
                known = true;
                dependencies.push_back(binding->second);
              }
            } else if constexpr (std::is_same_v<T, FunctionCall>) {
              if (operation.function < module.functions.size()) {
                known = true;
                dependencies.push_back(module.functions[operation.function].result);
              }
            } else if constexpr (std::is_same_v<T, ShapeCheck>) {
              known = true;
              dependencies.push_back(operation.input);
            } else if constexpr (std::is_same_v<T, ConditionalOperation>) {
              known = true;
              dependencies = {operation.trueResult, operation.falseResult};
            }
          },
          module.expressions[id].operation);
      if (!finish && !dependencies.empty()) {
        if (states[id] == 1)
          continue; // A cycle remains unproven, never assumed fresh.
        states[id] = 1;
        pending.push_back({id, true});
        for (auto dependency : dependencies)
          if (dependency < states.size() && states[dependency] == 0)
            pending.push_back({dependency});
      } else {
        for (auto dependency : dependencies)
          known &= dependency < states.size() && states[dependency] == 2 && fresh[dependency];
        fresh[id] = known;
        states[id] = 2;
      }
    }
    return bool(fresh[root]);
  };
  std::vector<bool> bodyNeeds(module.functions.size()), wrapperNeeds(module.functions.size());
  std::vector<std::vector<FunctionId>> callers(module.functions.size());
  for (FunctionId function = 0; function < module.functions.size(); ++function) {
    const auto &fn = module.functions[function];
    if (fn.resultType.array && fn.resultType.builtin == BuiltinValue::Numeric)
      wrapperNeeds[function] = !isFresh(fn.result);
    const auto opaqueInput = [&](ValueId id) {
      if (id >= module.expressions.size()) {
        bodyNeeds[function] = true;
        return;
      }
      const auto &type = module.expressions[id].type;
      if (type.array && type.builtin == BuiltinValue::Numeric && !isFresh(id))
        bodyNeeds[function] = true;
    };
    const auto calls = [&](FunctionId callee) {
      if (callee < callers.size())
        callers[callee].push_back(function);
      else
        bodyNeeds[function] = true;
    };
    for (auto id : fn.expressions) {
      if (id >= module.expressions.size()) {
        bodyNeeds[function] = true;
        continue;
      }
      std::visit(
          [&](const auto &operation) {
            using T = std::decay_t<decltype(operation)>;
            if constexpr (std::is_same_v<T, FunctionReference>)
              for (auto value : operation.captures)
                opaqueInput(value);
            else if constexpr (std::is_same_v<T, FunctionInvoke>)
              for (auto value : operation.arguments)
                opaqueInput(value);
            else if constexpr (std::is_same_v<T, RecordValue>)
              for (const auto &field : operation.fields)
                opaqueInput(field.value);
            else if constexpr (std::is_same_v<T, FunctionCall>)
              calls(operation.function);
            else if constexpr (std::is_same_v<T, ArrayMap>)
              calls(operation.function);
          },
          module.expressions[id].operation);
    }
  }
  // A direct private memref return is still a borrow. Only an opaque escape
  // inside the callee requires callers to normalize their incoming host data.
  std::vector<FunctionId> pending;
  for (FunctionId function = 0; function < bodyNeeds.size(); ++function)
    if (bodyNeeds[function])
      pending.push_back(function);
  while (!pending.empty()) {
    const auto function = pending.back();
    pending.pop_back();
    for (auto caller : callers[function])
      if (!bodyNeeds[caller]) {
        bodyNeeds[caller] = true;
        pending.push_back(caller);
      }
  }
  for (FunctionId function = 0; function < wrapperNeeds.size(); ++function)
    wrapperNeeds[function] = wrapperNeeds[function] || bodyNeeds[function];
  return wrapperNeeds;
}
} // namespace kalandra
