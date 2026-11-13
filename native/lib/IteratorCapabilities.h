#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
iteratorCapabilityError(const TypedModule &module, const TypedExpr &expr, NumericTarget target) {
  if (const auto *generator = std::get_if<GeneratorCreate>(&expr.operation)) {
    if (target != NumericTarget::Cpu)
      return "generator frames require host execution";
    if (!isIterator(module, expr.type) || expr.type.fallible || expr.type.task ||
        expr.type.device || expr.type.array || expr.type.tensor || expr.type.errorRecord ||
        !expr.type.shape.empty() || generator->function >= module.functions.size())
      return "invalid generator construction type or producer";
    const auto &function = module.functions[generator->function];
    const auto &cursor = module.records[*expr.type.record];
    if (!function.generator || cursor.typeArguments.size() != 2 ||
        function.parameters.size() != generator->captures.size() ||
        function.captureCount != generator->captures.size() ||
        !sameFunctionValueType(function.generator->item, cursor.typeArguments[0]))
      return "generator captures or item type disagree with its producer";
    for (std::size_t index = 0; index < generator->captures.size(); ++index)
      if (generator->captures[index] >= module.expressions.size() ||
          function.parameters[index] >= module.symbols.size() ||
          module.symbols[function.parameters[index]].owner != generator->function ||
          !module.symbols[function.parameters[index]].parameter ||
          module.symbols[function.parameters[index]].mutableAccess ||
          module.symbols[function.parameters[index]].owned !=
              isOwnedResource(module, module.expressions[generator->captures[index]].type) ||
          !sameFunctionValueType(module.expressions[generator->captures[index]].type,
                                 module.symbols[function.parameters[index]].type))
        return "generator capture changes its source type";
    const auto &result = function.resultType;
    if (result.builtin != BuiltinValue::Record || result.array || result.tensor || result.task ||
        result.device || !result.shape.empty() || !result.record ||
        *result.record >= module.records.size())
      return "generator producer must return its typed Option envelope";
    const auto &option = module.records[*result.record];
    if (option.standard != StandardRecord::Option || option.typeArguments.size() != 1 ||
        !sameFunctionValueType(option.typeArguments[0], function.generator->item))
      return "generator producer has an incompatible Option envelope";
    const auto &error = cursor.typeArguments[1];
    const bool empty = error.record && *error.record < module.records.size() &&
                       module.records[*error.record].uninhabited;
    const bool task = error.builtin == BuiltinValue::TaskError;
    if (result.fallible == empty ||
        result.errorRecord != (empty || task ? std::optional<std::size_t>{} : error.record))
      return "generator producer and cursor error channels disagree";
    return {};
  }
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
  if (unsigned(operation->action) > unsigned(IteratorAction::FromArray) ||
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
  if (operation->action == IteratorAction::FromArray) {
    const auto *result = cursor(expr.type);
    if (!result || !input.array || !input.tensor || input.shape.size() != 1 || input.device ||
        input.task || input.fallible || input.errorRecord)
      return bad();
    auto item = input;
    item.array = item.tensor = false;
    item.shape.clear();
    const auto &error = result->typeArguments[1];
    if (!sameFunctionValueType(result->typeArguments[0], item) || !error.record ||
        *error.record >= module.records.size() || !module.records[*error.record].uninhabited)
      return bad();
    return {};
  }
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
