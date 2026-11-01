#pragma once
#include "kalandra/GpuConversion.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/TypeUtilities.h"
// Vulkan's fallback artifact retains native binary32, but removes native
// binary64 conversions as well as arithmetic. Keep this out of Metal's default
// strict pass; its source emitter already generates integer conversion code.
inline mlir::LogicalResult lowerGpuFloat64Conversions(
    mlir::Operation *root, const char *diagnostic = "GPU float64 software conversion requires supported scalar numeric types") {
  using namespace mlir;
  using namespace kalandra;
  llvm::SmallVector<Operation *> operations;
  root->walk([&](Operation *op) {
    if (isa<arith::SIToFPOp, arith::UIToFPOp, arith::FPToSIOp, arith::FPToUIOp, arith::ExtFOp,
            arith::TruncFOp>(op) &&
        (getElementTypeOrSelf(op->getOperand(0).getType()).isF64() ||
         getElementTypeOrSelf(op->getResult(0).getType()).isF64()))
      operations.push_back(op);
  });
  const auto numeric = [](Type type, bool unsignedInteger) -> std::optional<NumericType> {
    if (type.isF32())
      return NumericType::Float32;
    if (type.isF64())
      return NumericType::Float64;
    if (auto integer = dyn_cast<IntegerType>(type))
      for (auto description : numericTypes)
        if (isInteger(description.type) && description.bits == integer.getWidth() &&
            isUnsigned(description.type) == unsignedInteger)
          return description.type;
    return {};
  };
  for (auto *op : operations) {
    const auto source = numeric(op->getOperand(0).getType(), isa<arith::UIToFPOp>(op));
    const auto destination = numeric(op->getResult(0).getType(), isa<arith::FPToUIOp>(op));
    if (!source || !destination) {
      op->emitError(diagnostic);
      return failure();
    }
    OpBuilder builder(op);
    auto loc = op->getLoc();
    Value bits = op->getOperand(0);
    if (!isInteger(*source))
      bits = arith::BitcastOp::create(builder, loc,
                                      builder.getIntegerType(describeNumeric(*source)->bits), bits);
    if (describeNumeric(*source)->bits < 64)
      bits = arith::ExtUIOp::create(builder, loc, builder.getI64Type(), bits);
    auto converted =
        gpuNumericConversion(builder, loc, bits, *source, *destination, ArithmeticMode::Software);
    auto zero = arith::ConstantIntOp::create(builder, loc, 0, 64);
    Value result = arith::SelectOp::create(builder, loc, converted.second, converted.first, zero);
    if (describeNumeric(*destination)->bits < 64)
      result = arith::TruncIOp::create(
          builder, loc, builder.getIntegerType(describeNumeric(*destination)->bits), result);
    if (!isInteger(*destination))
      result = arith::BitcastOp::create(builder, loc, op->getResult(0).getType(), result);
    op->getResult(0).replaceAllUsesWith(result);
    op->erase();
  }
  return success();
}

// Preserve Vulkan's existing entry point and diagnostic while sharing the
// backend-independent integer conversion algorithm with CUDA.
inline mlir::LogicalResult lowerVulkanFloat64Conversions(mlir::Operation *root) {
  return lowerGpuFloat64Conversions(root, "Vulkan float64 fallback conversion requires supported scalar numeric types");
}
