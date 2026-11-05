#pragma once
#include "DeviceTensorLayout.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include <stdexcept>

namespace kalandra {
struct DeviceTensorIndex {
  mlir::Value offset, valid;
};

// The caller sign-extends signed index values to i64. Unsigned comparison
// rejects both negative indices and unsigned indices outside the extent.
inline DeviceTensorIndex emitDeviceTensorIndex(mlir::OpBuilder &b, const DeviceTensorLayout &layout,
                                               std::span<const mlir::Value> indices) {
  using namespace mlir;
  if (indices.size() != layout.extents.size() || indices.size() != layout.strides.size())
    throw std::invalid_argument("invalid checkpointed tensor index rank");
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t value) -> Value {
    return arith::ConstantIntOp::create(b, loc, value, 64);
  };
  Value offset = c(0), valid = arith::ConstantIntOp::create(b, loc, 1, 1);
  for (std::size_t axis = 0; axis < indices.size(); ++axis) {
    const auto index = indices[axis];
    if (!index.getType().isInteger(64))
      throw std::invalid_argument("checkpointed tensor index requires i64");
    const Value bounded = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult,
                                                index, c(layout.extents[axis]));
    const Value safe = arith::SelectOp::create(b, loc, bounded, index, c(0));
    const Value displacement = arith::MulIOp::create(b, loc, safe, c(layout.strides[axis]));
    offset = arith::AddIOp::create(b, loc, offset, displacement);
    valid = arith::AndIOp::create(b, loc, valid, bounded);
  }
  return {offset, valid};
}
} // namespace kalandra
