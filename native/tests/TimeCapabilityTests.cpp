#include "../lib/TimeCapabilities.h"
#include <cstdio>
#include <cstdlib>
using namespace kalandra;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "time capability assertion %d: %s\n", __LINE__, #x);                    \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
int main() {
  TypedModule module;
  ValueType u64;
  u64.element = NumericType::UInt64;
  for (auto identity : {StandardRecord::Instant, StandardRecord::Duration}) {
    TypedRecord record{"opaque", {}, {}};
    record.standard = identity;
    record.fields.push_back(
        {identity == StandardRecord::Instant ? "__ticks" : "__nanoseconds", u64, {}});
    module.records.push_back(record);
  }
  TypedRecord error{"TimeError", {}, {}};
  error.error = true;
  error.standard = StandardRecord::TimeError;
  ValueType tag;
  tag.element = NumericType::Int64;
  error.fields.push_back({"$tag", tag, {}});
  error.variants = {{"Unavailable", {}, {}}, {"OutOfRange", {}, {}}};
  module.records.push_back(error);
  const auto nominal = [](unsigned id) {
    ValueType result;
    result.builtin = BuiltinValue::Record;
    result.record = id;
    return result;
  };
  for (unsigned kind = 1; kind <= 7; ++kind) {
    auto action = static_cast<TimeAction>(kind);
    auto input = action == TimeAction::Elapsed ? nominal(0)
                 : action == TimeAction::Nanoseconds || action == TimeAction::Milliseconds
                     ? u64
                     : nominal(1);
    module.expressions = {{{}, input, UInt64Literal{0}}};
    ValueType result = action == TimeAction::Now ? nominal(0) : nominal(1);
    if (action == TimeAction::AsNanoseconds)
      result = u64;
    if (action == TimeAction::AsSeconds) {
      result = {};
      result.element = NumericType::Float64;
    }
    if (action == TimeAction::Sleep) {
      result = {};
      result.builtin = BuiltinValue::Unit;
    }
    result.fallible = kind == 1 || kind == 2 || kind == 4 || kind == 7;
    if (result.fallible)
      result.errorRecord = 2;
    TimeOperation operation{action, action == TimeAction::Now ? std::vector<ValueId>{}
                                                              : std::vector<ValueId>{0}};
    TypedExpr expression{{}, result, operation};
    CHECK(!timeCapabilityError(module, expression, NumericTarget::Cpu));
    CHECK(timeCapabilityError(module, expression, NumericTarget::AppleGpu));
    auto invalid = expression;
    invalid.type.fallible = !result.fallible;
    CHECK(timeCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<TimeOperation>(invalid.operation).arguments.push_back(999);
    CHECK(timeCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    std::get<TimeOperation>(invalid.operation).action = static_cast<TimeAction>(99);
    CHECK(timeCapabilityError(module, invalid, NumericTarget::Cpu));
    invalid = expression;
    invalid.type.tensor = true;
    CHECK(timeCapabilityError(module, invalid, NumericTarget::Cpu));
    if (action != TimeAction::Now) {
      module.expressions[0].type.task = true;
      CHECK(timeCapabilityError(module, expression, NumericTarget::Cpu));
      module.expressions[0].type = input;
    }
    if (result.fallible) {
      module.records[2].standard.reset();
      CHECK(timeCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[2] = error;
    }
    if (input.record && action != TimeAction::Now) {
      auto record = module.records[*input.record];
      module.records[*input.record].standard.reset();
      CHECK(timeCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[*input.record] = record;
      module.records[*input.record].fields[0].type.element = NumericType::Int64;
      CHECK(timeCapabilityError(module, expression, NumericTarget::Cpu));
      module.records[*input.record] = record;
    }
  }
  std::puts("PASS: time capability contracts and opaque nominal identities");
}
