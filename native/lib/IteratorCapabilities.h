#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
iteratorCapabilityError(const TypedModule &module, const TypedExpr &expr, NumericTarget target) {
  const auto *operation = std::get_if<IteratorOperation>(&expr.operation);
  if (!operation)
    return {};
  if (target != NumericTarget::Cpu)
    return "CPU iterator frames require host execution; device iteration needs device-local "
           "lowering";
  const auto bad = []() {
    return std::optional<std::string>(
        "iterator operation requires its canonical cursor, item and error types");
  };
  if (unsigned(operation->action) > unsigned(IteratorAction::Close) ||
      operation->arguments.size() != 1 || operation->arguments[0] >= module.expressions.size())
    return bad();
  const auto scalar = [](const ValueType &type) {
    return !type.task && !type.fallible && !type.device && !type.tensor && !type.array &&
           !type.errorRecord && type.shape.empty();
  };
  const auto cursor = [&](const ValueType &type) -> const TypedRecord * {
    return scalar(type) && isIterator(module, type) &&
                   module.records[*type.record].typeArguments.size() == 2
               ? &module.records[*type.record]
               : nullptr;
  };
  const auto &input = module.expressions[operation->arguments[0]].type;
  if (operation->action == IteratorAction::FromRange) {
    const auto *result = cursor(expr.type);
    if (!result || !scalar(input) || !input.record || *input.record >= module.records.size() ||
        module.records[*input.record].standard != StandardRecord::Range)
      return bad();
    const auto &item = result->typeArguments[0], &error = result->typeArguments[1];
    if (!scalar(item) || item.builtin != BuiltinValue::Numeric ||
        item.element != NumericType::Int64 || !error.record ||
        *error.record >= module.records.size() || !module.records[*error.record].uninhabited)
      return bad();
    return {};
  }
  const auto *source = cursor(input);
  if (!source)
    return bad();
  if (operation->action == IteratorAction::Close)
    return scalar(expr.type) && expr.type.builtin == BuiltinValue::Unit ? std::nullopt : bad();
  const auto &item = source->typeArguments[0], &error = source->typeArguments[1];
  auto result = expr.type;
  result.fallible = false;
  result.errorRecord.reset();
  if (!scalar(result) || !result.record || *result.record >= module.records.size())
    return bad();
  const auto &option = module.records[*result.record];
  if (option.standard != StandardRecord::Option || option.typeArguments.size() != 1 ||
      !sameFunctionValueType(option.typeArguments[0], item))
    return bad();
  if (error.record && *error.record >= module.records.size())
    return bad();
  const bool empty = error.record && module.records[*error.record].uninhabited;
  const bool task = error.builtin == BuiltinValue::TaskError ||
                    (error.record && module.records[*error.record].taskErrorFamily);
  const auto family = empty || task ? std::optional<std::size_t>{} : error.record;
  if (expr.type.fallible == empty || expr.type.errorRecord != family)
    return bad();
  return {};
}
} // namespace kalandra
