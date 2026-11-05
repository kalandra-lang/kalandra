#pragma once
#include "DeviceTensorIndex.h"

namespace kalandra {
struct DeviceTensorShape {
  mlir::Value elements, valid;
};

inline DeviceTensorShape emitDeviceTensorShape(mlir::OpBuilder &b,
                                               std::span<const mlir::Value> extents,
                                               std::uint64_t maxWords) {
  using namespace mlir;
  if (extents.size() > 8 || !maxWords || maxWords > UINT32_MAX)
    throw std::invalid_argument("invalid checkpointed tensor rank or budget");
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t value, unsigned bits = 64) -> Value {
    return arith::ConstantIntOp::create(b, loc, value, bits);
  };
  Value valid = c(1, 1), empty = c(0, 1);
  for (const auto extent : extents) {
    if (!extent.getType().isInteger(64))
      throw std::invalid_argument("checkpointed tensor extent requires i64");
    const Value inRange = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule,
                                                extent, c(maxWords));
    const Value zero = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, extent, c(0));
    valid = arith::AndIOp::create(b, loc, valid, inRange);
    empty = arith::OrIOp::create(b, loc, empty, zero);
  }
  Value elements = arith::SelectOp::create(b, loc, empty, c(0), c(1));
  for (const auto extent : extents) {
    // Both factors are bounded by UINT32_MAX, so their i64 product is exact.
    const Value safe = arith::SelectOp::create(b, loc, valid, extent, c(0));
    const Value product = arith::MulIOp::create(b, loc, elements, safe);
    const Value fits = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, product, c(maxWords));
    valid = arith::AndIOp::create(b, loc, valid, fits);
    elements = arith::SelectOp::create(b, loc, valid, product, c(0));
  }
  return {elements, valid};
}

inline DeviceTensorIndex emitDynamicDeviceTensorIndex(mlir::OpBuilder &b,
                                                      std::span<const mlir::Value> extents,
                                                      std::span<const mlir::Value> indices,
                                                      mlir::Value shapeValid) {
  using namespace mlir;
  if (indices.size() != extents.size())
    throw std::invalid_argument("invalid checkpointed tensor index rank");
  const auto loc = b.getUnknownLoc();
  const Value zero = arith::ConstantIntOp::create(b, loc, 0, 64);
  Value valid = shapeValid, offset = zero;
  for (std::size_t axis = 0; axis < indices.size(); ++axis) {
    if (!indices[axis].getType().isInteger(64))
      throw std::invalid_argument("checkpointed tensor index requires i64");
    const Value bounded = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult,
                                                indices[axis], extents[axis]);
    valid = arith::AndIOp::create(b, loc, valid, bounded);
    const Value extent = arith::SelectOp::create(b, loc, valid, extents[axis], zero);
    const Value index = arith::SelectOp::create(b, loc, valid, indices[axis], zero);
    offset = arith::AddIOp::create(b, loc, arith::MulIOp::create(b, loc, offset, extent), index);
  }
  return {offset, valid};
}
} // namespace kalandra
