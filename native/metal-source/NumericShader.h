#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/Semantics.h"
#include <expected>
#include <span>
namespace kalandra {
struct NumericShader {
  NumericType element;
  std::string mlir;
  std::vector<std::uint32_t> reads;
  std::string reduction{};
  bool matrix = false;
  bool scalar = false;
  std::vector<NumericType> tensorTypes{}, scalarTypes{};
  std::vector<std::uint8_t> scalarBuffers{};
  bool indirect = false;
  std::optional<TensorTransformKind> transform = {};
  std::string nativeMetal = {};
  bool noArithmeticFault = false;
};
// Typed map and primitive shaders use integer bit carriers at the task boundary.
std::string numericReductionShader(NumericType);
struct NumericShaderOptions {
  std::span<const std::uint8_t> deviceScalarParameters{};
  bool boolResult = false;
  ArithmeticMode arithmetic = ArithmeticMode::Hardware;
  bool nativeFloat64Conversions = false;
};
std::expected<NumericShader, Diagnostic> emitNumericShader(const TypedModule &, FunctionId,
                                                           const NumericShaderOptions & = {});
} // namespace kalandra
