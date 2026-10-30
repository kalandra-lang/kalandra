#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/NumericType.h"
#include "mlir/IR/Builders.h"
#include <utility>
namespace kalandra {
// Checked conversion under the selected arithmetic policy. Input/output are canonical i64
// bit carriers; validity must be checked before publishing the destination.
std::pair<mlir::Value, mlir::Value> gpuNumericConversion(mlir::OpBuilder &, mlir::Location,
                                                         mlir::Value, NumericType, NumericType,
                                                         ArithmeticMode = ArithmeticMode::Hardware);
} // namespace kalandra
