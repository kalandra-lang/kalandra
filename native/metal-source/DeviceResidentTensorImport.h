#pragma once
#include "DeviceHeap.h"
#include "DeviceTensorDynamic.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

namespace kalandra {
// One device-side word copy per call. The input allocation remains resident
// and read-only. The new handle becomes visible only after the whole copy.
inline mlir::Value emitResidentTensorImport(mlir::OpBuilder &b, DeviceHeapEmitter &heap,
    mlir::Value buffer, mlir::Value sealedCount, std::span<const mlir::Value> extents,
    unsigned destination, unsigned layout) {
  using namespace mlir;
  using B = runtime::DeviceHeapBlockABI;
  const auto bufferType = dyn_cast<MemRefType>(buffer.getType());
  if (!bufferType || bufferType.getRank() != 1 ||
      !isa<IntegerType>(bufferType.getElementType()) || extents.size() > 8 ||
      !sealedCount.getType().isInteger(64) || layout < B::TensorLayoutBase ||
      layout >= B::TensorLayoutEnd ||
      (layout - B::TensorLayoutBase) % B::TensorRankCount != extents.size())
    throw std::invalid_argument("invalid checkpointed resident tensor binding");
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t value, unsigned bits = 64) -> Value {
    return arith::ConstantIntOp::create(b, loc, value, bits);
  };
  const auto add = [&](Value left, Value right) -> Value {
    return arith::AddIOp::create(b, loc, left, right);
  };
  const auto eq = [&](Value left, Value right) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, left, right);
  };
  const auto when = [&](Value condition, auto body) {
    auto branch = scf::IfOp::create(b, loc, condition, false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    body();
  };
  const auto rank = c(extents.size());
  const auto shape = emitDeviceTensorShape(b, extents,
      runtime::detail::ControlArenaABI::MaxWords - 4 - extents.size());
  const Value valid = arith::AndIOp::create(b, loc, shape.valid, eq(sealedCount, shape.elements));
  const auto words = add(sealedCount, rank);
  const auto ready = heap.ready();
  const auto cursor = heap.cursor();
  const Value done = arith::AndIOp::create(b, loc, valid,
      arith::AndIOp::create(b, loc, ready, eq(cursor, words)));
  when(done, [&] {
    heap.store(destination, heap.handle());
    heap.finish();
  });
  const Value copying = arith::AndIOp::create(b, loc, valid,
      arith::AndIOp::create(b, loc, ready, arith::CmpIOp::create(
          b, loc, arith::CmpIPredicate::ult, cursor, words)));
  when(copying, [&] {
    const auto address = add(c(4), cursor);
    for (std::size_t axis = 0; axis < extents.size(); ++axis)
      when(eq(cursor, c(axis)), [&] { heap.write(heap.handle(), address, extents[axis]); });
    when(arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, cursor, rank), [&] {
      const Value element = arith::SubIOp::create(b, loc, cursor, rank);
      const Value index = arith::IndexCastOp::create(b, loc, b.getIndexType(), element);
      Value bits = memref::LoadOp::create(b, loc, buffer, ValueRange{index});
      if (!bits.getType().isInteger(64))
        bits = arith::ExtUIOp::create(b, loc, b.getI64Type(), bits);
      heap.write(heap.handle(), address, bits);
    });
    heap.advance();
  });
  const Value starting = arith::AndIOp::create(b, loc, valid,
      eq(heap.load(DeviceHeapEmitter::Phase), c(0)));
  // Do not start another allocation after finish changed the phase.
  when(arith::AndIOp::create(b, loc, starting,
      arith::XOrIOp::create(b, loc, ready, c(1, 1))), [&] {
    heap.begin(words, layout);
  });
  when(arith::XOrIOp::create(b, loc, valid, c(1, 1)), [&] { heap.fault(99); });
  return done;
}
} // namespace kalandra
