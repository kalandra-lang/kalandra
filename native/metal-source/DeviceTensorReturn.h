#pragma once
#include "DeviceHeap.h"
#include "DeviceTensorDynamic.h"
#include "../gpu-runtime/ControlTensorExport.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

namespace kalandra {
inline void emitDeviceTensorReturn(DeviceHeapEmitter &heap, mlir::OpBuilder &b,
                                   mlir::Value root, const ValueType &type) {
  using namespace mlir;
  using A = runtime::detail::ControlTensorExportABI;
  using B = runtime::DeviceHeapBlockABI;
  static_assert(A::Root == DeviceHeapEmitter::Cells);
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t value, unsigned bits = 64) -> Value {
    return arith::ConstantIntOp::create(b, loc, value, bits);
  };
  const auto cmp = [&](arith::CmpIPredicate predicate, Value lhs, Value rhs) -> Value {
    return arith::CmpIOp::create(b, loc, predicate, lhs, rhs);
  };
  const auto both = [&](Value lhs, Value rhs) -> Value {
    return arith::AndIOp::create(b, loc, lhs, rhs);
  };
  const auto rank = type.shape.size();
  const auto capacity = heap.load(DeviceHeapEmitter::Capacity);
  const Value remaining = arith::SubIOp::create(b, loc, capacity, root);
  const auto bounded = both(cmp(arith::CmpIPredicate::ne, root, c(0)),
      both(cmp(arith::CmpIPredicate::ult, root, capacity),
           cmp(arith::CmpIPredicate::uge, remaining, c(4 + rank))));
  auto branch = scf::IfOp::create(b, loc, bounded, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  const auto span = heap.read(root, c(0));
  const auto layout = heap.read(root, c(1));
  const auto words = heap.read(root, c(2));
  std::vector<Value> extents, operands;
  Value matches = c(1, 1);
  for (std::size_t axis = 0; axis < rank; ++axis) {
    const auto extent = heap.read(root, c(4 + axis));
    extents.push_back(extent);
    operands.push_back(type.shape[axis].constant ? c(*type.shape[axis].constant) : extent);
    if (type.shape[axis].constant)
      matches = both(matches, cmp(arith::CmpIPredicate::eq, extent, operands.back()));
  }
  const auto shape = emitDeviceTensorShape(b, operands,
      runtime::detail::ControlArenaABI::MaxWords - 4 - rank);
  const Value required = arith::AddIOp::create(b, loc, shape.elements, c(rank));
  const auto valid = both(both(matches, shape.valid),
      both(cmp(arith::CmpIPredicate::eq, layout, c(B::tensorLayout(type.element, rank))),
           both(cmp(arith::CmpIPredicate::eq, words, required),
                both(cmp(arith::CmpIPredicate::uge, span,
                         arith::AddIOp::create(b, loc, required, c(4))),
                     cmp(arith::CmpIPredicate::ule, span, remaining)))));
  auto publish = scf::IfOp::create(b, loc, valid, true);
  b.setInsertionPointToStart(&publish.getThenRegion().front());
  heap.store(A::Root, root);
  heap.store(A::Data, arith::AddIOp::create(b, loc, root, c(4 + rank)));
  heap.store(A::Length, shape.elements);
  for (std::size_t axis = 0; axis < rank; ++axis)
    heap.store(A::Extents + axis, extents[axis]);
  b.setInsertionPointToStart(&publish.getElseRegion().front());
  heap.fault(99);
  b.setInsertionPointToStart(&branch.getElseRegion().front());
  heap.fault(99);
}
} // namespace kalandra
