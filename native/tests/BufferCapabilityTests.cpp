#include "../lib/BufferCapabilities.h"
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "buffer capability assertion %d: %s\n", __LINE__, #x);                  \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  TypedModule module;
  ValueType integer;
  integer.element = NumericType::Int64;
  ValueType byte;
  byte.element = NumericType::UInt8;
  auto bytes = byte;
  bytes.array = bytes.tensor = true;
  bytes.shape.resize(1);
  TypedRecord buffer{"ByteBuffer", {}, {}};
  buffer.standard = StandardRecord::ByteBuffer;
  buffer.fields.push_back({"__storage", bytes, {}});
  TypedRecord error{"BufferError", {}, {}};
  error.standard = StandardRecord::BufferError;
  error.error = true;
  error.fields.push_back({"$tag", integer, {}});
  error.variants = {{"InvalidSize", {}, {}}, {"OutOfRange", {}, {}}};
  module.records = {buffer, error};
  ValueType nominal;
  nominal.builtin = BuiltinValue::Record;
  nominal.record = 0;
  for (unsigned raw = 0; raw <= unsigned(BufferAction::WriteByte); ++raw) {
    const auto action = BufferAction(raw);
    const bool index = action == BufferAction::CheckIndex || action == BufferAction::ReadByte ||
                       action == BufferAction::WriteByte;
    auto first = action == BufferAction::Create ? integer : nominal;
    auto second = action == BufferAction::Append ? bytes : integer;
    module.expressions = {
        {{}, first, Int64Literal{0}}, {{}, second, Int64Literal{0}}, {{}, byte, Int32Literal{0}}};
    std::vector<ValueId> arguments{0};
    if (index || action == BufferAction::Reserve || action == BufferAction::Append ||
        action == BufferAction::Truncate)
      arguments.push_back(1);
    if (action == BufferAction::WriteByte)
      arguments.push_back(2);
    ValueType result;
    result.builtin = BuiltinValue::Unit;
    if (action == BufferAction::Create)
      result = nominal;
    if (action == BufferAction::Length || action == BufferAction::Capacity ||
        action == BufferAction::CheckIndex)
      result = integer;
    if (action == BufferAction::ReadByte)
      result = byte;
    if (action == BufferAction::WriteByte)
      result.builtin = BuiltinValue::Bool;
    if (action == BufferAction::ToArray || action == BufferAction::IntoArray)
      result = bytes;
    result.fallible = fallibleBuffer(action);
    if (result.fallible)
      result.errorRecord = 1;
    TypedExpr expression{{}, result, BufferOperation{action, arguments, {}}};
    CHECK(!bufferCapabilityError(module, expression, NumericTarget::Cpu));
    CHECK(bufferCapabilityError(module, expression, NumericTarget::AppleGpu));
    auto invalid = expression;
    invalid.type.task = true;
    CHECK(bufferCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.shape.resize(2);
    CHECK(bufferCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.fallible = !result.fallible;
    CHECK(bufferCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<BufferOperation>(invalid.operation).arguments[0] = 999;
    CHECK(bufferCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<BufferOperation>(invalid.operation).action = BufferAction(99);
    CHECK(bufferCapabilityError(module, invalid, NumericTarget::Cpu));
    module.expressions[0].type.task = true;
    CHECK(bufferCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = first;
    if (action != BufferAction::Create) {
      module.records[0].fields[0].type.element = NumericType::Int64;
      CHECK(bufferCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[0] = buffer;
    }
    if (result.fallible) {
      module.records[1].variants[0].name = "Wrong";
      CHECK(bufferCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[1] = error;
    }
  }
  std::puts("ByteBuffer: all twelve capability contracts and malformed layouts passed");
}
