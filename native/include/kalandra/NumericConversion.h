#pragma once
#include "kalandra/Arithmetic.h"
#include "kalandra/NumericType.h"
#include "mlir/IR/Builders.h"
#include <utility>
namespace kalandra {
// Checked conversion returns {converted value, valid}, using the selected arithmetic policy.
// The caller must guard validity
// before publishing the value. All shifts are bounded, including invalid input paths.
std::pair<mlir::Value, mlir::Value>
numericConversion(mlir::OpBuilder &builder, mlir::Location loc, mlir::Value input,
                  NumericType source, NumericType destination,
                  ArithmeticMode arithmetic = ArithmeticMode::Hardware);
} // namespace kalandra
