#pragma once
#include "DeviceClosureImport.h"
#include "kalandra/Arithmetic.h"
#include "kalandra/Semantics.h"
#include "kalandra/WorkgroupLaunch.h"
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
  bool deviceStorage = false;
  bool checkpointedTensorArithmetic = false;
  std::optional<WorkgroupLaunch> workgroup = {};
};
// Typed map and primitive shaders use integer bit carriers at the task boundary.
std::string numericReductionShader(NumericType);
struct NumericShaderOptions {
  std::span<const std::uint8_t> deviceScalarParameters{};
  bool boolResult = false;
  ArithmeticMode arithmetic = ArithmeticMode::Hardware;
  bool nativeFloat64Conversions = false;
};
struct DeviceGraphExportLayout;
bool usesScalarControl(const TypedModule &, FunctionId);
std::expected<NumericShader, Diagnostic> emitScalarControlShader(const TypedModule &, FunctionId,
                                                                 ArithmeticMode,
                                                                 bool nativeFloat64Conversions,
                                                                 bool aggregateEnvelope = false,
                                                                 std::optional<std::size_t> graphParameter = {},
                                                                 std::span<const DeviceGraphExportLayout> graphResultLayouts = {},
                                                               std::span<const std::size_t> graphPeers = {},
                                                               std::span<const DeviceClosureImport> closureImports = {});
std::expected<NumericShader, Diagnostic> emitNumericShader(const TypedModule &, FunctionId,
                                                           const NumericShaderOptions & = {});
std::expected<NumericShader, Diagnostic> emitWorkgroupShader(const TypedModule &, FunctionId,
                                                             const NumericShaderOptions & = {});
} // namespace kalandra
