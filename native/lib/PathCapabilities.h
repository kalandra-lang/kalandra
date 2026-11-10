#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
pathCapabilityError(const TypedModule &module, const TypedExpr &expression, NumericTarget target) {
  const auto *op = std::get_if<PathOperation>(&expression.operation);
  if (!op)
    return {};
  const auto bad = [](const char *message) -> std::optional<std::string> {
    return std::string("path operations ") + message;
  };
  if (target != NumericTarget::Cpu)
    return bad("require host CPU execution");
  if (unsigned(op->action) < 1 || unsigned(op->action) > 5 ||
      op->input >= module.expressions.size())
    return bad("have an invalid action or input");
  const auto &input = module.expressions[op->input].type;
  const auto plain = [](const ValueType &type) {
    return !type.task && !type.fallible && !type.errorRecord && !type.device;
  };
  if (!plain(input) || input.builtin != BuiltinValue::String)
    return bad("require completed String input");
  if (op->action == PathAction::Join) {
    if (!input.array || !input.tensor || input.shape.size() != 1)
      return bad("join requires a rank-one String array");
  } else if (input.array || input.tensor || !input.shape.empty())
    return bad("require a scalar String input");
  const auto &result = expression.type;
  if (!plain(result) || result.array || result.tensor || !result.shape.empty() ||
      result.builtin !=
          (op->action == PathAction::IsAbsolute ? BuiltinValue::Bool : BuiltinValue::String))
    return bad("have an invalid result type");
  return {};
}
} // namespace kalandra
