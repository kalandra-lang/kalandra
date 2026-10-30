#pragma once
#include "RecordPhysicalLayout.h"
#include <unordered_set>

namespace kalandra {
inline bool hasRecursiveStorage(const RecordStoragePlan &storage) {
  return std::ranges::find(storage.recursive, true) != storage.recursive.end();
}
inline bool recursiveValueTypeSupported(const ValueType &root) {
  std::vector<const ValueType *> pending{&root};
  std::size_t work = 0;
  while (!pending.empty()) {
    const auto &type = *pending.back();
    pending.pop_back();
    if (++work > 300000 || type.device || type.builtin == BuiltinValue::Device)
      return false;
    if (type.builtin == BuiltinValue::Function) {
      if (((type.tensor || type.array) && !isFunctionArray(type)) || !type.callable)
        return false;
      pending.push_back(&type.callable->result);
      for (const auto &parameter : type.callable->parameters)
        pending.push_back(&parameter);
    }
  }
  return true;
}
// Recursive CPU values use cycle-safe storage and ownership; placement stays separate.
inline std::optional<Diagnostic> recursiveRecordLayoutError(const TypedModule &module,
                                                            const SemanticLimits &limits = {}) {
  auto storage = planRecordStorage(module);
  if (!storage)
    return storage.error();
  if (!hasRecursiveStorage(*storage))
    return recordLayoutError(module, limits);
  if (auto invalid = recordFieldError(module))
    return invalid;
  for (std::size_t id = 0; id < module.records.size(); ++id) {
    const auto &record = module.records[id];
    if (!storage->constructible[id]) {
      auto site = record.declaration;
      for (const auto &field : record.fields)
        if (field.type.record && !storage->constructible[*field.type.record]) {
          site = field.declaration;
          break;
        }
      return Diagnostic{
          "S068", "recursive type cycle has no finite construction: '" + record.name + "'", site};
    }
    for (const auto &field : record.fields)
      if ((field.type.tensor && field.type.builtin != BuiltinValue::Numeric &&
           !isBoolArray(field.type) && !isStringArray(field.type) && !isRecordArray(field.type) &&
           !isFunctionArray(field.type)) ||
          (field.type.builtin != BuiltinValue::Numeric &&
           field.type.builtin != BuiltinValue::Bool && field.type.builtin != BuiltinValue::String &&
           field.type.builtin != BuiltinValue::Record &&
           field.type.builtin != BuiltinValue::Function &&
           field.type.builtin != BuiltinValue::TaskError) ||
          !recursiveValueTypeSupported(field.type))
        return Diagnostic{
            "S068",
            "recursive programs require numeric/Bool/String scalars, numeric "
            "collections, Bool/String/record arrays "
            "or callables, function arrays, TaskError values and nominal links; other "
            "payloads remain unsupported",
            field.declaration};
  }
  auto physical = planRecordPhysicalLayout(module, limits);
  return physical ? std::optional<Diagnostic>{} : std::optional<Diagnostic>{physical.error()};
}
inline std::optional<std::string> recursiveProgramError(const TypedModule &module) {
  auto storage = planRecordStorage(module);
  if (!storage)
    return storage.error().message;
  if (!hasRecursiveStorage(*storage))
    return {};
  if (auto invalid = recursiveRecordLayoutError(module))
    return invalid->message;
  const auto supported = recursiveValueTypeSupported;
  for (const auto &symbol : module.symbols)
    if (!supported(symbol.type))
      return "recursive programs require CPU values without device residency";
  for (const auto &function : module.functions)
    if (!supported(function.resultType) ||
        (function.declaredResult && !supported(*function.declaredResult)))
      return "recursive programs require CPU function signatures without device residency";
  for (const auto &expression : module.expressions) {
    if (!supported(expression.type))
      return "recursive programs require CPU values without device residency";
    if (std::holds_alternative<DeviceSelection>(expression.operation) ||
        std::holds_alternative<Transfer>(expression.operation))
      return "recursive values require CPU execution";
    if (const auto *match = std::get_if<MatchOperation>(&expression.operation)) {
      if (match->subject >= module.expressions.size() || match->cases.empty())
        return "invalid recursive match subject/cases";
      const auto &subject = module.expressions[match->subject].type;
      if (subject.tensor || subject.task || subject.fallible ||
          (subject.builtin != BuiltinValue::Record && subject.builtin != BuiltinValue::TaskError &&
           subject.builtin != BuiltinValue::FaultKind))
        return "recursive matches require completed enum or error subjects";
      if (subject.builtin == BuiltinValue::Record &&
          (!subject.record || *subject.record >= module.records.size()))
        return "invalid recursive match nominal identity";
      const auto count = subject.builtin == BuiltinValue::Record
                             ? module.records[*subject.record].variants.size()
                         : subject.builtin == BuiltinValue::TaskError ? 4u
                                                                      : 5u;
      if (!count || match->cases.size() > count)
        return "invalid recursive match case count";
      std::unordered_set<unsigned> seen;
      bool fallback = false;
      for (std::size_t index = 0; index < match->cases.size(); ++index) {
        const auto &arm = match->cases[index];
        if (arm.result >= module.expressions.size())
          return "invalid recursive match result";
        if (arm.tag) {
          if (!*arm.tag || *arm.tag > count || !seen.insert(*arm.tag).second || fallback)
            return "invalid recursive match tag";
        } else {
          if (fallback || index + 1 != match->cases.size())
            return "invalid recursive match fallback";
          fallback = true;
        }
        for (const auto &statement : arm.body)
          if (statement.value >= module.expressions.size() ||
              (statement.binding && *statement.binding >= module.symbols.size()))
            return "invalid recursive match statement";
      }
      if (!fallback && seen.size() != count)
        return "non-exhaustive recursive match";
    }
  }
  return {};
}
} // namespace kalandra
