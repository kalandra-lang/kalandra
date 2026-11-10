#pragma once
#include "kalandra/Semantics.h"

namespace kalandra {
inline std::optional<std::string> environmentCapabilityError(const TypedModule &module,
                                                             const TypedExpr &expression,
                                                             NumericTarget target) {
  const auto *lookup = std::get_if<EnvironmentLookup>(&expression.operation);
  if (!lookup)
    return {};
  if (target != NumericTarget::Cpu)
    return "environment lookup requires host CPU execution";
  const auto plain = [](const ValueType &type) {
    return !type.tensor && !type.array && !type.task && !type.fallible && !type.device &&
           !type.errorRecord && type.shape.empty();
  };
  const auto string = [&](const ValueType &type) {
    return plain(type) && type.builtin == BuiltinValue::String && !type.record;
  };
  if (lookup->input >= module.expressions.size() || !string(module.expressions[lookup->input].type))
    return "environment lookup requires a completed String name";
  const auto &result = expression.type;
  if (!result.fallible || !result.errorRecord || *result.errorRecord >= module.records.size() ||
      result.builtin != BuiltinValue::Record || !result.record ||
      *result.record >= module.records.size() || result.array || result.tensor || result.task ||
      result.device || !result.shape.empty())
    return "environment lookup requires EnvValue with an EnvError channel";

  const auto tag = [&](const TypedRecord &record) {
    return !record.fields.empty() && record.fields[0].name == "$tag" &&
           plain(record.fields[0].type) && !record.fields[0].type.record &&
           record.fields[0].type.builtin == BuiltinValue::Numeric &&
           record.fields[0].type.element == NumericType::Int64;
  };
  const auto &value = module.records[*result.record];
  if (value.standard != StandardRecord::EnvValue || value.error || value.variants.size() != 2 ||
      value.fields.size() != 2 || !tag(value) || value.variants[0].name != "Missing" ||
      !value.variants[0].fields.empty() || value.variants[1].name != "Value" ||
      value.variants[1].fields.size() != 1 || value.variants[1].fields[0] != 1 ||
      value.fields[1].name != "value" || !string(value.fields[1].type))
    return "environment lookup has an invalid EnvValue layout";
  const auto &error = module.records[*result.errorRecord];
  if (error.standard != StandardRecord::EnvError || !error.error || error.variants.size() != 2 ||
      error.fields.size() != 1 || !tag(error) || error.variants[0].name != "InvalidName" ||
      error.variants[1].name != "InvalidEncoding" || !error.variants[0].fields.empty() ||
      !error.variants[1].fields.empty())
    return "environment lookup has an invalid EnvError layout";
  return {};
}
} // namespace kalandra
