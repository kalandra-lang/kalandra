#pragma once
#include "kalandra/FormatABI.h"
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<runtime::FormatScalarKind> formatScalarKind(const ValueType &type) {
  using K = runtime::FormatScalarKind;
  if (type.tensor || type.array || type.task || type.fallible || type.device)
    return {};
  if (type.builtin == BuiltinValue::Bool)
    return K::Bool;
  if (type.builtin != BuiltinValue::Numeric)
    return {};
  switch (type.element) {
  case NumericType::Int8:
    return K::I8;
  case NumericType::Int16:
    return K::I16;
  case NumericType::Int32:
    return K::I32;
  case NumericType::Int64:
    return K::I64;
  case NumericType::UInt8:
    return K::U8;
  case NumericType::UInt16:
    return K::U16;
  case NumericType::UInt32:
    return K::U32;
  case NumericType::UInt64:
    return K::U64;
  case NumericType::Float32:
    return K::F32;
  case NumericType::Float64:
    return K::F64;
  }
  return {};
}
} // namespace kalandra
