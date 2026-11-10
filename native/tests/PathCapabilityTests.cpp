#include "../lib/PathCapabilities.h"
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "path capability assertion %d: %s\n", __LINE__, #x);                    \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  TypedModule module;
  for (unsigned action = 1; action <= 5; ++action) {
    ValueType input;
    input.builtin = BuiltinValue::String;
    if (action == 1) {
      input.array = input.tensor = true;
      input.shape.resize(1);
    }
    module.expressions = {{{}, input, UInt64Literal{0}}};
    ValueType result;
    result.builtin = action == 5 ? BuiltinValue::Bool : BuiltinValue::String;
    TypedExpr expression{{}, result, PathOperation{static_cast<PathAction>(action), 0}};
    CHECK(!pathCapabilityError(module, expression, NumericTarget::Cpu));
    CHECK(pathCapabilityError(module, expression, NumericTarget::AppleGpu));
    auto invalid = expression;
    invalid.type.fallible = true;
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.errorRecord = 0;
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.builtin = BuiltinValue::Numeric;
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.tensor = true;
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<PathOperation>(invalid.operation).input = 999;
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<PathOperation>(invalid.operation).action = static_cast<PathAction>(0);
    CHECK(pathCapabilityError(module, invalid, NumericTarget::Cpu));
    module.expressions[0].type.task = true;
    CHECK(pathCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = input;
    module.expressions[0].type.builtin = BuiltinValue::Numeric;
    CHECK(pathCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = input;
    module.expressions[0].type.shape.resize(2);
    CHECK(pathCapabilityError(module, expression, NumericTarget::Cpu));
    module.expressions[0].type = input;
    module.expressions[0].type.array = !input.array;
    CHECK(pathCapabilityError(module, expression, NumericTarget::Cpu));
  }
  std::puts("Path capability validation: all five operations and malformed contracts passed");
}
