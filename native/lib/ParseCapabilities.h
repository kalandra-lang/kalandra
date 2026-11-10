#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
parseCapabilityError(const TypedModule &module, const TypedExpr &expression, NumericTarget target) {
  const auto *op = std::get_if<ParseOperation>(&expression.operation);
  if (!op)
    return {};
  const auto bad = [](const char *text) -> std::optional<std::string> {
    return std::string("text parsing ") + text;
  };
  if (target != NumericTarget::Cpu)
    return bad("requires host CPU execution");
  const auto plain = [](const ValueType &t) {
    return !t.tensor && !t.array && !t.task && !t.fallible && !t.device && t.shape.empty();
  };
  if (unsigned(op->kind) < 1 || unsigned(op->kind) > 11 || op->input >= module.expressions.size())
    return bad("has an invalid scalar kind or input");
  const auto &input = module.expressions[op->input].type;
  if (!plain(input) || input.builtin != BuiltinValue::String)
    return bad("requires a completed String input");
  auto result = expression.type;
  if (!result.fallible || !result.errorRecord || *result.errorRecord >= module.records.size())
    return bad("requires its nominal ParseError channel");
  const auto &error = module.records[*result.errorRecord];
  if (error.standard != StandardRecord::ParseError || !error.error || error.variants.size() != 2 ||
      error.variants[0].name != "Invalid" || error.variants[1].name != "OutOfRange" ||
      error.variants[0].fields.size() != 1 || !error.variants[1].fields.empty() ||
      error.fields.size() != 2 || error.fields[0].name != "$tag" || !plain(error.fields[0].type) ||
      error.fields[0].type.builtin != BuiltinValue::Numeric ||
      error.fields[0].type.element != NumericType::Int64)
    return bad("has an invalid ParseError layout");
  const auto offset = error.variants[0].fields[0];
  if (offset != 1 || offset >= error.fields.size() || error.fields[offset].name != "offset" ||
      !plain(error.fields[offset].type) ||
      error.fields[offset].type.builtin != BuiltinValue::Numeric ||
      error.fields[offset].type.element != NumericType::UInt64)
    return bad("requires a u64 Invalid offset");
  result.fallible = false;
  result.errorRecord.reset();
  if (!plain(result))
    return bad("requires a scalar result");
  if (op->kind == runtime::ParseScalarKind::Bool) {
    if (result.builtin != BuiltinValue::Bool)
      return bad("requires a bool result");
  } else {
    constexpr NumericType types[] = {NumericType::Int8,   NumericType::Int16,  NumericType::Int32,
                                     NumericType::Int64,  NumericType::UInt8,  NumericType::UInt16,
                                     NumericType::UInt32, NumericType::UInt64, NumericType::Float32,
                                     NumericType::Float64};
    if (result.builtin != BuiltinValue::Numeric || result.element != types[unsigned(op->kind) - 1])
      return bad("result does not match the scalar kind");
  }
  return {};
}
} // namespace kalandra
