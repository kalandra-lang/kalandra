#pragma once
#include "kalandra/Semantics.h"
namespace kalandra {
inline std::optional<std::string>
fileCapabilityError(const TypedModule &module, const TypedExpr &expression, NumericTarget target) {
  const auto *op = std::get_if<FileOperation>(&expression.operation);
  if (!op)
    return {};
  const auto bad = [](const char *message) -> std::optional<std::string> {
    return std::string("filesystem I/O ") + message;
  };
  const auto scalar = [](const ValueType &t) {
    return !t.tensor && !t.array && !t.task && !t.fallible && !t.device && t.shape.empty();
  };
  const auto nominal = [&](const ValueType &t, StandardRecord record) {
    return t.builtin == BuiltinValue::Record && t.record && *t.record < module.records.size() &&
           module.records[*t.record].standard == record;
  };
  const auto bytes = [](const ValueType &t) {
    return t.tensor && t.array && t.shape.size() == 1 && t.builtin == BuiltinValue::Numeric &&
           t.element == NumericType::UInt8 && !t.task && !t.fallible && !t.device;
  };
  const auto u64 = [&](const ValueType &t) {
    return scalar(t) && t.builtin == BuiltinValue::Numeric && t.element == NumericType::UInt64;
  };
  auto result = expression.type;
  if (target != NumericTarget::Cpu || result.task || result.device || !result.fallible ||
      !result.errorRecord || *result.errorRecord >= module.records.size() ||
      module.records[*result.errorRecord].standard != StandardRecord::FsError)
    return bad("requires host execution and its nominal FsError channel");
  if (unsigned(op->action) > unsigned(FileAction::WriteLine))
    return bad("has an invalid action");
  const auto count = op->action == FileAction::Flush || op->action == FileAction::Finish ||
                             op->action == FileAction::Close
                         ? 1u
                         : 2u;
  if (op->arguments.size() != count)
    return bad("has an invalid argument count");
  for (auto id : op->arguments)
    if (id >= module.expressions.size())
      return bad("references invalid input");
  const auto &first = module.expressions[op->arguments[0]].type;
  if (op->action == FileAction::Open) {
    const auto &mode = module.expressions[op->arguments[1]].type;
    if (!scalar(first) || first.builtin != BuiltinValue::String || !scalar(mode) ||
        !nominal(mode, StandardRecord::OpenMode))
      return bad("open requires String and OpenMode");
  } else {
    if (!scalar(first) || !isFileResource(module, first))
      return bad("requires a completed opaque host handle");
    const bool reading = op->action == FileAction::Read || op->action == FileAction::ReadExact ||
                         op->action == FileAction::ReadInto || op->action == FileAction::ReadLine;
    const bool writing = op->action == FileAction::Write || op->action == FileAction::WriteAll ||
                         op->action == FileAction::WriteText ||
                         op->action == FileAction::WriteLine || op->action == FileAction::Flush;
    if ((reading && nominal(first, StandardRecord::BufferedWriter)) ||
        (writing && nominal(first, StandardRecord::BufferedReader)) ||
        ((op->action == FileAction::BufferedReader || op->action == FileAction::BufferedWriter) &&
         !nominal(first, StandardRecord::File)) ||
        (op->action == FileAction::Finish && !nominal(first, StandardRecord::BufferedWriter)) ||
        (op->action == FileAction::Close && nominal(first, StandardRecord::BufferedWriter)))
      return bad("has an incompatible handle kind");
    if (count > 1) {
      const auto &second = module.expressions[op->arguments[1]].type;
      if (op->action == FileAction::ReadInto || op->action == FileAction::Write ||
          op->action == FileAction::WriteAll) {
        if (!bytes(second))
          return bad("requires a completed host [u8] array");
      } else if (op->action == FileAction::WriteText || op->action == FileAction::WriteLine) {
        if (!scalar(second) || second.builtin != BuiltinValue::String)
          return bad("text output requires a String");
      } else if (!u64(second))
        return bad("requires a u64 count, capacity or offset");
    }
  }
  result.fallible = false;
  result.errorRecord.reset();
  switch (op->action) {
  case FileAction::Open:
    if (!scalar(result) || !nominal(result, StandardRecord::File))
      return bad("open requires a File result");
    break;
  case FileAction::BufferedReader:
  case FileAction::BufferedWriter:
    if (!scalar(result) ||
        !nominal(result, op->action == FileAction::BufferedReader ? StandardRecord::BufferedReader
                                                                  : StandardRecord::BufferedWriter))
      return bad("buffer construction has an invalid result");
    break;
  case FileAction::Read:
  case FileAction::ReadExact:
    if (!bytes(result))
      return bad("byte reads require a [u8] result");
    break;
  case FileAction::ReadInto:
  case FileAction::Write:
    if (!u64(result))
      return bad("counted operations require a u64 result");
    break;
  case FileAction::ReadLine:
    if (!scalar(result) || !nominal(result, StandardRecord::Line))
      return bad("line reads require a Line result");
    break;
  default:
    if (!scalar(result) || result.builtin != BuiltinValue::Unit)
      return bad("requires a unit result");
  }
  return {};
}
} // namespace kalandra
