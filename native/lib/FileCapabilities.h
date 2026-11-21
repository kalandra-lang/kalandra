#pragma once
#include "kalandra/TypeCapabilities.h"
#include <algorithm>
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
    return typeFacts(t).completedHostScalar();
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
  if (op->action >= FileAction::StreamRead && op->action <= FileAction::Capture) {
    const bool process = op->action >= FileAction::Start;
    const bool read =
        op->action == FileAction::StreamRead || op->action == FileAction::StreamAppend;
    const bool write =
        op->action == FileAction::StreamWrite || op->action == FileAction::StreamWriteAll;
    if (!hostExecutionAvailable(target) || result.task || result.device || !result.fallible ||
        !result.errorRecord || *result.errorRecord >= module.records.size() ||
        module.records[*result.errorRecord].standard !=
            (process ? StandardRecord::ProcessError : StandardRecord::StreamError))
      return bad("requires a host stream or process error channel");
    const unsigned count = read ? 3 : write || op->action == FileAction::Capture ? 2 : 1;
    if (op->arguments.size() != count || std::ranges::any_of(op->arguments, [&](auto id) {
          return id >= module.expressions.size();
        }))
      return bad("invalid stream or process arguments");
    const auto &first = module.expressions[op->arguments[0]].type;
    const auto arg = [&](unsigned i) -> const ValueType & {
      return module.expressions[op->arguments[i]].type;
    };
    result.fallible = false;
    result.errorRecord.reset();
    if (!scalar(first) || !scalar(result))
      return bad("requires completed scalar handles or records");
    bool valid = false;
    if (read || write) {
      valid =
          (nominal(first, StandardRecord::File) ||
           nominal(first, read ? StandardRecord::BufferedReader : StandardRecord::BufferedWriter) ||
           nominal(first, read ? StandardRecord::PipeReader : StandardRecord::PipeWriter)) &&
          scalar(arg(1)) && isByteBuffer(module, arg(1)) && (!read || u64(arg(2))) &&
          (op->action == FileAction::StreamWriteAll ? result.builtin == BuiltinValue::Unit
                                                    : u64(result));
    } else if (op->action == FileAction::PipeClose) {
      valid = (nominal(first, StandardRecord::PipeReader) ||
               nominal(first, StandardRecord::PipeWriter)) &&
              result.builtin == BuiltinValue::Unit;
    } else if (commandAction(op->action)) {
      valid = nominal(first, StandardRecord::Command) &&
              (op->action == FileAction::Start
                   ? nominal(result, StandardRecord::Child)
                   : u64(arg(1)) && nominal(result, StandardRecord::Captured));
    } else if (nominal(first, StandardRecord::Child)) {
      valid =
          (op->action == FileAction::Wait && nominal(result, StandardRecord::ProcessStatus)) ||
          (op->action == FileAction::TakeStdin && nominal(result, StandardRecord::PipeWriter)) ||
          ((op->action == FileAction::TakeStdout || op->action == FileAction::TakeStderr) &&
           nominal(result, StandardRecord::PipeReader));
    }
    return valid ? std::nullopt : bad("invalid stream or process operation signature");
  }
  if (!hostExecutionAvailable(target) || result.task || result.device || !result.fallible ||
      !result.errorRecord || *result.errorRecord >= module.records.size() ||
      module.records[*result.errorRecord].standard != StandardRecord::FsError)
    return bad("requires host execution and its nominal FsError channel");
  if (unsigned(op->action) > unsigned(FileAction::ReadWatch))
    return bad("has an invalid action");
  if (op->action >= FileAction::OpenDirectory) {
    const bool watchOpen = op->action == FileAction::OpenWatch;
    if (op->arguments.size() != (watchOpen ? 2u : 1u) ||
        op->arguments.front() >= module.expressions.size())
      return bad("utility operation requires one valid input");
    if (watchOpen) {
      const auto scope = op->arguments[1];
      if (scope >= module.expressions.size() || !scalar(module.expressions[scope].type) ||
          !nominal(module.expressions[scope].type, StandardRecord::WatchScope))
        return bad("watchDir requires WatchScope");
    }
    const auto &argument = module.expressions[op->arguments[0]].type;
    if (!scalar(argument) ||
        (filePathAction(op->action) ? argument.builtin != BuiltinValue::String
                                    : !nominal(argument, StandardRecord::File)))
      return bad("utility operation has an invalid path or directory handle");
    result.fallible = false;
    result.errorRecord.reset();
    if (!scalar(result))
      return bad("utility operation requires a completed scalar result");
    if (op->action == FileAction::ReadWatch)
      return nominal(result, StandardRecord::WatchEvent) ? std::nullopt : bad("watch advance requires WatchEvent");
    if (watchOpen || op->action == FileAction::OpenDirectory || op->action == FileAction::OpenWalk)
      return nominal(result, StandardRecord::File) ? std::nullopt
                                                   : bad("directory open requires File");
    if (op->action == FileAction::Metadata)
      return nominal(result, StandardRecord::FileInfo) ? std::nullopt
                                                       : bad("metadata requires FileInfo");
    if (op->action == FileAction::ReadDirectory) {
      if (!nominal(result, StandardRecord::Option))
        return bad("directory advance requires Option[DirEntry]");
      const auto &fields = module.records[*result.record].fields;
      const auto item =
          std::ranges::find_if(fields, [](const auto &field) { return field.name == "value"; });
      return item != fields.end() && scalar(item->type) && nominal(item->type, StandardRecord::DirEntry)
                 ? std::nullopt
                 : bad("directory option requires DirEntry");
    }
    return result.builtin == BuiltinValue::Unit ? std::nullopt
                                                : bad("path modification requires unit");
  }
  const auto count =
      op->action == FileAction::Flush || op->action == FileAction::Finish ||
              op->action == FileAction::Close
          ? 1u
      : op->action == FileAction::ReadIntoBuffer || op->action == FileAction::ReadAppend ? 3u
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
    if (!scalar(first) ||
        !(nominal(first, StandardRecord::File) || nominal(first, StandardRecord::BufferedReader) ||
          nominal(first, StandardRecord::BufferedWriter)))
      return bad("requires a completed opaque host handle");
    const bool reading = op->action == FileAction::Read || op->action == FileAction::ReadExact ||
                         op->action == FileAction::ReadInto ||
                         op->action == FileAction::ReadIntoBuffer ||
                         op->action == FileAction::ReadAppend || op->action == FileAction::ReadLine;
    const bool writing = op->action == FileAction::Write || op->action == FileAction::WriteAll ||
                         op->action == FileAction::WriteBuffer ||
                         op->action == FileAction::WriteAllBuffer ||
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
      if (op->action == FileAction::ReadIntoBuffer || op->action == FileAction::ReadAppend ||
          op->action == FileAction::WriteBuffer || op->action == FileAction::WriteAllBuffer) {
        if (!scalar(second) || !isByteBuffer(module, second) ||
            (count == 3 && !u64(module.expressions[op->arguments[2]].type)))
          return bad("requires ByteBuffer and a u64 read limit");
      } else if (op->action == FileAction::ReadInto || op->action == FileAction::Write ||
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
  case FileAction::ReadIntoBuffer:
  case FileAction::ReadAppend:
  case FileAction::WriteBuffer:
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
