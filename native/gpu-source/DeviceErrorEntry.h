#pragma once

#include "DeviceErrorLayout.h"
#include <array>

namespace kalandra {
// Construct a private two-variant result for the existing GPU aggregate
// transport. Execution faults still terminate the device invocation through
// its status channel. Only an expected application error reaches the handler.
inline std::expected<FunctionId, std::string>
appendDeviceErrorEntry(TypedModule &module, FunctionId function, std::size_t &budget) {
  if (function >= module.functions.size())
    return std::unexpected("GPU error entry requires a checked function");
  const auto &original = module.functions[function];
  const auto result = original.resultType;
  if (result.task || !result.fallible || !result.errorRecord || original.captureCount)
    return std::unexpected("GPU error entry requires a named application-fallible function");
  const std::array families{*result.errorRecord};
  auto layouts = planDeviceErrorPayloadLayouts(module, families, budget);
  if (!layouts)
    return std::unexpected(layouts.error());
  if (budget < 16 || original.parameters.size() > (budget - 16) / 4)
    return std::unexpected("GPU error entry exceeds configured work budget");
  budget -= 16 + 4 * original.parameters.size();

  auto success = result;
  success.fallible = false;
  success.errorRecord.reset();
  ValueType error;
  error.builtin = BuiltinValue::Record;
  error.record = result.errorRecord;
  ValueType tag;
  tag.element = NumericType::Int64;
  ValueType outcome;
  outcome.builtin = BuiltinValue::Record;
  outcome.record = module.records.size();
  const auto name = "$gpu.outcome." + std::to_string(function);
  const auto span = original.declaration;
  module.records.push_back({name, span,
                            {{"$tag", tag, span}, {"value", success, span}, {"error", error, span}},
                            {{"Success", span, {1}}, {"Failure", span, {2}}}});
  auto fallibleOutcome = outcome;
  fallibleOutcome.fallible = true;
  fallibleOutcome.errorRecord = result.errorRecord;

  const auto innerId = module.functions.size();
  const auto outerId = innerId + 1;
  const auto create = [&](FunctionId owner, const ValueType &type, std::string suffix) {
    TypedFunction wrapper;
    wrapper.name = name + suffix;
    wrapper.declaration = span;
    wrapper.resultType = type;
    wrapper.declaredResult = type;
    wrapper.preconditions = original.preconditions;
    for (auto parameter : original.parameters) {
      auto symbol = module.symbols.at(parameter);
      symbol.owner = owner;
      wrapper.parameters.push_back(module.symbols.size());
      module.symbols.push_back(std::move(symbol));
    }
    return wrapper;
  };
  auto inner = create(innerId, fallibleOutcome, ".pack");
  auto outer = create(outerId, outcome, ".export");
  const auto add = [&](TypedFunction &wrapper, const ValueType &type, auto operation) {
    const auto id = module.expressions.size();
    module.expressions.push_back({span, type, std::move(operation)});
    wrapper.expressions.push_back(id);
    return id;
  };
  const auto arguments = [&](TypedFunction &wrapper) {
    std::vector<ValueId> values;
    values.reserve(wrapper.parameters.size());
    for (auto symbol : wrapper.parameters)
      values.push_back(add(wrapper, module.symbols[symbol].type, SymbolRead{symbol}));
    return values;
  };
  const auto call = add(inner, result, FunctionCall{function, arguments(inner)});
  const auto value = add(inner, success, TaskOperation{TaskPrefix::Try, call});
  inner.result = add(inner, outcome, RecordValue{{}, {{1, value}}, 0});
  inner.body = {{span, {}, inner.result}};

  const auto packed = add(outer, fallibleOutcome, FunctionCall{innerId, arguments(outer)});
  const auto binding = module.symbols.size();
  module.symbols.push_back({"$error", error, span, outerId, false});
  const auto read = add(outer, error, SymbolRead{binding});
  const auto failed = add(outer, outcome, RecordValue{{}, {{2, read}}, 1});
  outer.result = add(outer, outcome, CatchOperation{packed, binding, {{span, {}, failed}}, failed});
  outer.body = {{span, {}, outer.result}};
  module.functions.push_back(std::move(inner));
  module.functions.push_back(std::move(outer));
  return outerId;
}
} // namespace kalandra
