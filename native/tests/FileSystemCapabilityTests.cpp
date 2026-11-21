#include "../lib/FileCapabilities.h"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "file capability assertion %d: %s\n", __LINE__, #x);                    \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  TypedModule module;
  constexpr std::array records{StandardRecord::File,           StandardRecord::BufferedReader,
                               StandardRecord::BufferedWriter, StandardRecord::OpenMode,
                               StandardRecord::FsError,        StandardRecord::Line,
                               StandardRecord::FileKind,       StandardRecord::DirEntry,
                               StandardRecord::FileInfo,       StandardRecord::Option,
                               StandardRecord::WatchScope, StandardRecord::WatchEvent};
  for (auto standard : records) {
    TypedRecord record{"standard", {}, {}};
    record.standard = standard;
    record.error = standard == StandardRecord::FsError;
    module.records.push_back(record);
  }
  const auto nominal = [](std::size_t id) {
    ValueType type;
    type.builtin = BuiltinValue::Record;
    type.record = id;
    return type;
  };
  module.records[9].fields.push_back({"value", nominal(7), {}});
  ValueType count;
  count.element = NumericType::UInt64;
  ValueType text;
  text.builtin = BuiltinValue::String;
  ValueType bytes;
  bytes.array = bytes.tensor = true;
  bytes.element = NumericType::UInt8;
  bytes.shape.resize(1);
  ValueType unit;
  unit.builtin = BuiltinValue::Unit;
  struct Spec {
    FileAction action;
    std::vector<ValueType> arguments;
    ValueType result;
  };
  const std::array specs{Spec{FileAction::Open, {text, nominal(3)}, nominal(0)},
                         Spec{FileAction::BufferedReader, {nominal(0), count}, nominal(1)},
                         Spec{FileAction::BufferedWriter, {nominal(0), count}, nominal(2)},
                         Spec{FileAction::Read, {nominal(1), count}, bytes},
                         Spec{FileAction::ReadExact, {nominal(0), count}, bytes},
                         Spec{FileAction::ReadInto, {nominal(1), bytes}, count},
                         Spec{FileAction::ReadLine, {nominal(1), count}, nominal(5)},
                         Spec{FileAction::Write, {nominal(2), bytes}, count},
                         Spec{FileAction::WriteAll, {nominal(0), bytes}, unit},
                         Spec{FileAction::Flush, {nominal(2)}, unit},
                         Spec{FileAction::Finish, {nominal(2)}, unit},
                         Spec{FileAction::Close, {nominal(1)}, unit},
                         Spec{FileAction::SeekTo, {nominal(1), count}, unit},
                         Spec{FileAction::WriteText, {nominal(2), text}, unit},
                         Spec{FileAction::WriteLine, {nominal(0), text}, unit},
                         Spec{FileAction::OpenDirectory, {text}, nominal(0)},
                         Spec{FileAction::ReadDirectory, {nominal(0)}, nominal(9)},
                         Spec{FileAction::Metadata, {text}, nominal(8)},
                         Spec{FileAction::DirectoryCreate, {text}, unit},
                         Spec{FileAction::RemoveFile, {text}, unit},
                         Spec{FileAction::DirectoryRemove, {text}, unit},
                         Spec{FileAction::OpenWalk, {text}, nominal(0)},
                         Spec{FileAction::DirectoryCreateAll, {text}, unit},
                         Spec{FileAction::RemoveTree, {text}, unit},
                         Spec{FileAction::OpenWatch, {text, nominal(10)}, nominal(0)},
                         Spec{FileAction::ReadWatch, {nominal(0)}, nominal(11)}};
  for (const auto &spec : specs) {
    module.expressions.clear();
    std::vector<ValueId> arguments;
    for (auto type : spec.arguments) {
      arguments.push_back(module.expressions.size());
      module.expressions.push_back({{}, type, UInt64Literal{0}});
    }
    auto result = spec.result;
    result.fallible = true;
    result.errorRecord = 4;
    TypedExpr expression{{}, result, FileOperation{spec.action, arguments}};
    CHECK(!fileCapabilityError(module, expression, NumericTarget::Cpu));
    CHECK(fileCapabilityError(module, expression, NumericTarget::AppleGpu));
    if (spec.action == FileAction::ReadDirectory) {
      module.records[9].fields[0].type = nominal(5);
      CHECK(fileCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[9].fields[0].type = nominal(7);
    }
    auto invalid = expression;
    std::get<FileOperation>(invalid.operation).arguments.clear();
    CHECK(fileCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<FileOperation>(invalid.operation).action = static_cast<FileAction>(999);
    CHECK(fileCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.fallible = false;
    CHECK(fileCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.errorRecord.reset();
    CHECK(fileCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.builtin = BuiltinValue::Device;
    CHECK(fileCapabilityError(module, invalid, NumericTarget::Cpu));
    module.expressions[0].type.task = true;
    CHECK(fileCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = spec.arguments[0];
    std::get<FileOperation>(expression.operation).arguments[0] = 999;
    CHECK(fileCapabilityError(module, expression, NumericTarget::Cpu));
  }
  std::printf("Filesystem capability validation: %zu operations and invalid contracts passed\n",
              specs.size());
}
