#pragma once
#include "NumericShader.h"
#include "kalandra/GpuProgram.h"
namespace kalandra {
struct NumericProgramInstruction {
  NumericProgramKind kind;
  std::uint32_t kernel = UINT32_MAX;
  std::vector<std::uint32_t> inputs{}, scalarSources{};
  std::vector<std::uint64_t> constants{};
  std::uint32_t output = UINT32_MAX, target = 0;
  std::uint32_t workPerElement = 0;
};
struct NumericProgram {
  std::vector<NumericShader> kernels;
  std::vector<NumericProgramInstruction> code;
  std::uint32_t result;
  ValueType type;
};
std::expected<NumericProgram, Diagnostic>
emitNumericProgram(const TypedModule &, FunctionId, ArithmeticMode = ArithmeticMode::Hardware);
} // namespace kalandra
