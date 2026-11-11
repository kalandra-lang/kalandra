#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
timeCapabilityError(const TypedModule &module, const TypedExpr &expression, NumericTarget target) {
  const auto *operation = std::get_if<TimeOperation>(&expression.operation);
  if (!operation)
    return {};
  if (target != NumericTarget::Cpu)
    return "time operations require host CPU execution";
  const auto plain = [](const ValueType &type) {
    return !type.task && !type.fallible && !type.errorRecord && !type.device && !type.array &&
           !type.tensor && type.shape.empty();
  };
  const auto numeric = [&](const ValueType &type, NumericType numeric) {
    return plain(type) && !type.record && type.builtin == BuiltinValue::Numeric &&
           type.element == numeric;
  };
  const auto record = [&](const ValueType &type, StandardRecord standard) {
    if (!plain(type) || type.builtin != BuiltinValue::Record || !type.record ||
        *type.record >= module.records.size())
      return false;
    const auto &declaration = module.records[*type.record];
    return declaration.standard == standard && !declaration.error && declaration.variants.empty() &&
           declaration.fields.size() == 1 &&
           declaration.fields[0].name ==
               (standard == StandardRecord::Instant ? "__ticks" : "__nanoseconds") &&
           numeric(declaration.fields[0].type, NumericType::UInt64);
  };
  const auto action = operation->action;
  if (unsigned(action) < 1 || unsigned(action) > 7 ||
      operation->arguments.size() != (action == TimeAction::Now ? 0u : 1u))
    return "time operation has invalid arguments";
  if (!operation->arguments.empty()) {
    if (operation->arguments[0] >= module.expressions.size())
      return "time operation has an invalid input";
    const auto &input = module.expressions[operation->arguments[0]].type;
    const bool valid = action == TimeAction::Elapsed ? record(input, StandardRecord::Instant)
                       : action == TimeAction::Nanoseconds || action == TimeAction::Milliseconds
                           ? numeric(input, NumericType::UInt64)
                           : record(input, StandardRecord::Duration);
    if (!valid)
      return "time operation has an invalid input type";
  }
  auto result = expression.type;
  const bool fallible = action == TimeAction::Now || action == TimeAction::Elapsed ||
                        action == TimeAction::Milliseconds || action == TimeAction::Sleep;
  if (result.fallible != fallible)
    return "time operation has an invalid error contract";
  if (fallible) {
    if (!result.errorRecord || *result.errorRecord >= module.records.size())
      return "time operation requires TimeError";
    const auto &error = module.records[*result.errorRecord];
    if (error.standard != StandardRecord::TimeError || !error.error || error.fields.size() != 1 ||
        error.fields[0].name != "$tag" || !numeric(error.fields[0].type, NumericType::Int64) ||
        error.variants.size() != 2 || error.variants[0].name != "Unavailable" ||
        error.variants[1].name != "OutOfRange" || !error.variants[0].fields.empty() ||
        !error.variants[1].fields.empty())
      return "time operation has an invalid TimeError layout";
    result.fallible = false;
    result.errorRecord.reset();
  }
  const bool valid = action == TimeAction::Now ? record(result, StandardRecord::Instant)
                     : action == TimeAction::Sleep
                         ? plain(result) && !result.record && result.builtin == BuiltinValue::Unit
                     : action == TimeAction::AsSeconds ? numeric(result, NumericType::Float64)
                     : action == TimeAction::AsNanoseconds
                         ? numeric(result, NumericType::UInt64)
                         : record(result, StandardRecord::Duration);
  return valid ? std::nullopt : std::optional<std::string>("time operation has an invalid result");
}
} // namespace kalandra
