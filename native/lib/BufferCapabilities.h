#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
bufferCapabilityError(const TypedModule &module, const TypedExpr &expr, NumericTarget target) {
  const auto *op = std::get_if<BufferOperation>(&expr.operation);
  if (!op)
    return {};
  const auto bad = []() {
    return std::optional<std::string>("ByteBuffer operations require completed host values and "
                                      "their declared result/error types");
  };
  if (target != NumericTarget::Cpu || unsigned(op->action) > unsigned(BufferAction::WriteByte))
    return bad();
  const auto scalar = [](const ValueType &t) {
    return !t.array && !t.tensor && !t.task && !t.fallible && !t.device;
  };
  const auto bytes = [](const ValueType &t) {
    return t.array && t.tensor && t.builtin == BuiltinValue::Numeric &&
           t.element == NumericType::UInt8 && t.shape.size() == 1 && !t.task && !t.fallible &&
           !t.device;
  };
  const auto integer = [&](const ValueType &t) {
    return scalar(t) && t.builtin == BuiltinValue::Numeric && t.element == NumericType::Int64;
  };
  auto result = expr.type;
  if (result.fallible != fallibleBuffer(op->action) || result.task || result.device)
    return bad();
  if (result.fallible &&
      (!result.errorRecord || *result.errorRecord >= module.records.size() ||
       module.records[*result.errorRecord].standard != StandardRecord::BufferError))
    return bad();
  result.fallible = false;
  result.errorRecord.reset();
  const auto count = op->action == BufferAction::Reserve || op->action == BufferAction::Append ||
                             op->action == BufferAction::Truncate ||
                             op->action == BufferAction::CheckIndex ||
                             op->action == BufferAction::ReadByte
                         ? 2u
                     : op->action == BufferAction::WriteByte ? 3u
                                                             : 1u;
  if (op->arguments.size() != count)
    return bad();
  for (auto value : op->arguments)
    if (value >= module.expressions.size())
      return bad();
  const auto &first = module.expressions[op->arguments[0]].type;
  if (op->action == BufferAction::Create) {
    if (!integer(first) || !scalar(result) || !isByteBuffer(module, result))
      return bad();
  } else {
    if (!scalar(first) || !isByteBuffer(module, first))
      return bad();
    const bool indexing = op->action == BufferAction::CheckIndex ||
                          op->action == BufferAction::ReadByte ||
                          op->action == BufferAction::WriteByte;
    if (indexing) {
      const auto &index = module.expressions[op->arguments[1]].type;
      if (!scalar(index) || index.builtin != BuiltinValue::Numeric || !isInteger(index.element))
        return bad();
      if (op->action == BufferAction::WriteByte) {
        const auto &byte = module.expressions[op->arguments[2]].type;
        if (!scalar(byte) || byte.builtin != BuiltinValue::Numeric ||
            byte.element != NumericType::UInt8)
          return bad();
      }
    } else if (count == 2) {
      const auto &second = module.expressions[op->arguments[1]].type;
      if (op->action == BufferAction::Append ? !bytes(second) : !integer(second))
        return bad();
    }
    if (op->action == BufferAction::Length || op->action == BufferAction::Capacity ||
        op->action == BufferAction::CheckIndex) {
      if (!integer(result))
        return bad();
    } else if (op->action == BufferAction::ReadByte) {
      if (!scalar(result) || result.builtin != BuiltinValue::Numeric ||
          result.element != NumericType::UInt8)
        return bad();
    } else if (op->action == BufferAction::WriteByte) {
      if (!scalar(result) || result.builtin != BuiltinValue::Bool)
        return bad();
    } else if (op->action == BufferAction::ToArray || op->action == BufferAction::IntoArray) {
      if (!bytes(result))
        return bad();
    } else if (!scalar(result) || result.builtin != BuiltinValue::Unit)
      return bad();
  }
  return {};
}
} // namespace kalandra
