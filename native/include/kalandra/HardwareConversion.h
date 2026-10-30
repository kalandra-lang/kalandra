#pragma once
#include "kalandra/NumericType.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
namespace kalandra {
// Preserve checked conversion admission, but execute the admitted conversion on
// the target FPU. Invalid float-to-integer inputs must never reach LLVM fptosi/ui.
inline mlir::Value hardwareConversion(mlir::OpBuilder &b, mlir::Location loc, mlir::Value input,
                                      NumericType source, NumericType destination,
                                      mlir::Value valid) {
  using namespace mlir;
  const auto dst = describeNumeric(destination);
  Type type = isInteger(destination) ? Type(b.getIntegerType(dst->bits))
              : dst->bits == 32      ? Type(b.getF32Type())
                                     : Type(b.getF64Type());
  auto choice = scf::IfOp::create(b, loc, TypeRange{type}, valid, true, true);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&choice.getThenRegion().front());
    Value result;
    if (isInteger(source)) {
      result = isUnsigned(source) ? Value(arith::UIToFPOp::create(b, loc, type, input))
                                  : Value(arith::SIToFPOp::create(b, loc, type, input));
    } else if (isInteger(destination)) {
      result = isUnsigned(destination) ? Value(arith::FPToUIOp::create(b, loc, type, input))
                                       : Value(arith::FPToSIOp::create(b, loc, type, input));
    } else if (describeNumeric(source)->bits < dst->bits) {
      result = arith::ExtFOp::create(b, loc, type, input);
    } else {
      result = arith::TruncFOp::create(b, loc, type, input);
    }
    scf::YieldOp::create(b, loc, ValueRange{result});
    b.setInsertionPointToStart(&choice.getElseRegion().front());
    auto zero = arith::ConstantOp::create(b, loc, type, b.getZeroAttr(type));
    scf::YieldOp::create(b, loc, ValueRange{zero});
  }
  return choice.getResult(0);
}
} // namespace kalandra
