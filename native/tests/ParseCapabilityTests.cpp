#include "../lib/ParseCapabilities.h"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "parse capability assertion %d: %s\n", __LINE__, #x);                   \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  TypedModule module;
  ValueType offset;
  offset.element = NumericType::UInt64;
  TypedRecord error{"ParseError", {}, {}};
  error.standard = StandardRecord::ParseError;
  error.error = true;
  ValueType tag;
  tag.element = NumericType::Int64;
  error.fields.push_back({"$tag", tag, {}});
  error.fields.push_back({"offset", offset, {}});
  error.variants.push_back({"Invalid", {}, {1}});
  error.variants.push_back({"OutOfRange", {}, {}});
  module.records.push_back(error);
  ValueType text;
  text.builtin = BuiltinValue::String;
  module.expressions.push_back({{}, text, UInt64Literal{0}});
  constexpr std::array types{NumericType::Int8,   NumericType::Int16,  NumericType::Int32,
                             NumericType::Int64,  NumericType::UInt8,  NumericType::UInt16,
                             NumericType::UInt32, NumericType::UInt64, NumericType::Float32,
                             NumericType::Float64};
  for (unsigned kind = 1; kind <= 11; ++kind) {
    ValueType result;
    if (kind == 11)
      result.builtin = BuiltinValue::Bool;
    else
      result.element = types[kind - 1];
    result.fallible = true;
    result.errorRecord = 0;
    TypedExpr expression{
        {}, result, ParseOperation{static_cast<runtime::ParseScalarKind>(kind), 0}};
    CHECK(!parseCapabilityError(module, expression, NumericTarget::Cpu));
    CHECK(parseCapabilityError(module, expression, NumericTarget::AppleGpu));
    auto invalid = expression;
    invalid.type.fallible = false;
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.errorRecord.reset();
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.array = true;
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.builtin = BuiltinValue::String;
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<ParseOperation>(invalid.operation).input = 999;
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<ParseOperation>(invalid.operation).kind = static_cast<runtime::ParseScalarKind>(99);
    CHECK(parseCapabilityError(module, invalid, NumericTarget::Cpu));
    module.expressions[0].type.task = true;
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = text;
    module.records[0].standard.reset();
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.records[0] = error;
    module.records[0].fields[1].type.element = NumericType::Int64;
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.records[0] = error;
    module.records[0].fields[0].type.element = NumericType::UInt32;
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.records[0] = error;
    module.records[0].variants[0].fields = {0};
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.records[0] = error;
    module.records[0].variants[0].fields = {999};
    CHECK(parseCapabilityError(module, expression, NumericTarget::Cpu));
    module.records[0] = error;
  }
  std::puts("Parsing capability validation: all scalar types and invalid contracts passed");
}
