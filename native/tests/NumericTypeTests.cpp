#include "../metal/Metal.h"
#include "kalandra/Backend.h"
#include "kalandra/Continuation.h"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace kalandra;
namespace m = kalandra::runtime::metal;
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
static_assert(resolveNumeric("int") == NumericType::Int64);
static_assert(resolveNumeric("uint") == NumericType::UInt64);
static_assert(resolveNumeric("float") == NumericType::Float64);
static_assert(checkedNumericBytes(NumericType::UInt8, 3) == 3);
static_assert(checkedNumericBytes(NumericType::Float64, 3) == 24);
static_assert(!checkedNumericBytes(NumericType::Float64, UINT64_MAX));
CheckResult analyze(const std::string &text) {
  return check(parse(std::make_shared<SourceFile>("numeric.k", text)));
}
int main() {
  try {
    std::set<std::string_view> names;
    std::set<NumericType> identities;
    for (auto description : numericTypes) {
      require(names.insert(description.name).second && identities.insert(description.type).second,
              "duplicate numeric identity/name");
      require(resolveNumeric(description.name) == description.type &&
                  describeNumeric(description.type)->bits == description.bits,
              "numeric catalogue does not round-trip");
      const auto bytes = description.bytes();
      const auto count = UINT64_MAX / bytes;
      require(checkedNumericBytes(description.type, count) == count * bytes,
              "maximum byte count rejected");
      if (bytes > 1)
        require(!checkedNumericBytes(description.type, count + 1), "allocation size wrapped");
      require(checkedNumericBytes(description.type, 0, 0) == 0 &&
                  checkedNumericBytes(description.type, 3, bytes * 3) == bytes * 3 &&
                  !checkedNumericBytes(description.type, 3, bytes * 3 - 1),
              "allocation limit/empty semantics changed");
      ValueType type;
      type.element = description.type;
      require(describeType(type) == description.name, "scalar type lost numeric identity");
      type.task = type.fallible = true;
      require(describeType(type) == "task<" + std::string(description.name) + "> ! TaskError",
              "task/error type lost numeric identity");
      type.tensor = true;
      type.shape = {{3, 0}};
      require(describeType(type) ==
                  "Tensor[" + std::string(description.name) + ", 3] task ! TaskError",
              "tensor type lost numeric identity");
      for (auto target : {NumericTarget::Cpu, NumericTarget::AppleGpu})
        require(numericArithmeticAvailable(description.type, target) ==
                    (target == NumericTarget::AppleGpu ||
                     description.type == NumericType::Float32 ||
                     (target == NumericTarget::Cpu &&
                      (isInteger32(description.type) || description.type == NumericType::Int64 ||
                       description.type == NumericType::UInt64 ||
                       description.type == NumericType::Float64))),
                "type description accidentally enabled arithmetic");
      require(numericTensorConversionAvailable(description.type, NumericTarget::Cpu) &&
                  numericTensorConversionAvailable(description.type, NumericTarget::AppleGpu),
              "tensor conversion capability lost numeric identity");
      require(numericMatmulAvailable(description.type, NumericTarget::Cpu) &&
                  numericMatmulAvailable(description.type, NumericTarget::AppleGpu),
              "matmul capability lost numeric identity");
      require(numericReductionAvailable(description.type, NumericTarget::Cpu) &&
                  numericReductionAvailable(description.type, NumericTarget::AppleGpu),
              "reduction capability lost numeric identity");
      require(numericScalarArithmeticAvailable(description.type, NumericTarget::Cpu) &&
                  analyze("fn main() -> " + std::string(description.name) + ":\n  1\n").ok(),
              "accepted CPU scalar type rejected");
      require(numericScalarArithmeticAvailable(description.type, NumericTarget::AppleGpu),
              "typed GPU arithmetic identity rejected");
    }
    for (auto name : {"Number", "bool", "Float32", "int128", "float16", "bfloat16", "int0"})
      require(!resolveNumeric(name), "unknown/deferred numeric type silently mapped");
    const auto invalid = static_cast<NumericType>(255);
    require(!describeNumeric(invalid) && !checkedNumericBytes(invalid, 0) &&
                !numericArithmeticAvailable(invalid, NumericTarget::Cpu) &&
                !numericArithmeticAvailable(NumericType::Float32, static_cast<NumericTarget>(255)),
            "invalid type/target accepted");
    require(m::storageNumericType(m::Format::Float32) == NumericType::Float32 &&
                m::storageNumericType(m::Format::UInt32) == NumericType::UInt32 &&
                m::storageNumericType(m::Format::UInt8) == NumericType::UInt8 &&
                !m::storageNumericType(static_cast<m::Format>(255)),
            "private storage format mapping changed");
    require(numericArithmeticAvailable(*m::storageNumericType(m::Format::UInt32),
                                       NumericTarget::AppleGpu),
            "checked integer shader identity rejected");
    auto numeric = analyze("fn twice(x:float32) -> float32:\n  -x * -2\n"
                           "fn main() -> float32:\n  twice(3)\n");
    require(numeric.ok(), "float32 source rejected");
    require(!numericCapabilityError(*numeric.module, NumericTarget::Cpu),
            "float32 capability lost");
    auto lowered = lowerNative(*numeric.module);
    require(bool(lowered), "float32 lowering regressed");
    // All entry surfaces must reject unsupported numeric IR before float lowering.
    for (unsigned surface = 0; surface < 4; ++surface) {
      auto forged = *numeric.module;
      if (surface == 0) {
        forged.functions[0].resultType.element = static_cast<NumericType>(255);
        forged.functions[0].resultType.tensor = true;
      }
      if (surface == 1) {
        forged.functions[0].declaredResult->element = static_cast<NumericType>(255);
        forged.functions[0].declaredResult->tensor = true;
      }
      if (surface == 2) {
        forged.symbols[0].type.element = static_cast<NumericType>(255);
        forged.symbols[0].type.tensor = true;
      }
      if (surface == 3) {
        forged.expressions[0].type.element = NumericType::Int16;
        forged.expressions[0].operation = Float32Literal{0x3f800000u};
      }
      auto failure = lowerNative(forged);
      require(bool(numericCapabilityError(forged, NumericTarget::Cpu)) && !failure,
              "backend accepted unsupported numeric identity or malformed narrow literal");
    }
    auto task = analyze("fn identity(values:Tensor[float32,?]) -> Tensor[float32,?]:\n  values\n"
                        "fn main() -> float32 ! TaskError:\n  try parallel:\n"
                        "    values:Tensor[float32,2] = tensor([1,2])\n"
                        "    copied = try await spawn identity(values)\n    reduceSum(copied)\n");
    require(task.ok(), "task typing regressed");
    auto typed = *task.module;
    for (auto &fn : typed.functions) {
      fn.resultType.element = NumericType::UInt64;
      if (fn.declaredResult)
        fn.declaredResult->element = NumericType::UInt64;
    }
    for (auto &symbol : typed.symbols)
      symbol.type.element = NumericType::UInt64;
    for (auto &expr : typed.expressions)
      expr.type.element = NumericType::UInt64;
    auto planned = continuation::plan(typed);
    require(bool(planned), "structural planner cannot carry numeric identity");
    for (const auto &plan : planned->plans)
      for (const auto &slot : plan->slots)
        if (slot.builtin == BuiltinValue::Numeric)
          require(slot.element == NumericType::UInt64, "continuation lost numeric identity");
    auto unsupported = typed;
    for (auto &expr : unsupported.expressions)
      expr.type.element = static_cast<NumericType>(255);
    auto rejectedTask = lowerNative(unsupported);
    require(!rejectedTask && rejectedTask.error().find("<invalid>") != std::string::npos,
            "task backend accepted invalid numeric IR");
    auto realTask =
        analyze("fn identity(values:Tensor[float,?]) -> Tensor[float,?]:\n  values\n"
                "fn main() -> float ! TaskError:\n  try parallel:\n"
                "    values:Tensor[float,2] = tensor([1.0000000000000002,-0.0])\n"
                "    copied = try await spawn identity(values)\n    reduceSum(copied)\n");
    require(realTask.ok() && bool(lowerNative(*realTask.module)),
            "task backend rejected supported float64 reduction");
    ValueType builtin;
    builtin.builtin = BuiltinValue::TaskError;
    builtin.element = invalid;
    require(describeType(builtin) == "TaskError", "numeric metadata leaked into builtin type");
    std::cout
        << "10 concrete numeric descriptions: aliases, families, widths, checked storage sizes, "
           "typed continuation identity and strict CPU/GPU capability boundaries passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
