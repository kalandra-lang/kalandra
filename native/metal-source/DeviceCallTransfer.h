#pragma once
#include "DeviceHeap.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <stdexcept>

namespace kalandra {
struct DeviceCallMove {
  unsigned source, destination, words;
  unsigned outerSource = UINT32_MAX, outerDestination = UINT32_MAX;
};

// Emit one word per checkpoint. Frame endpoints are compiler-owned ranges
// verified at call/return admission. No collection or growth runs mid-copy.
// A null endpoint denotes the outer invocation parameter table.
inline mlir::Value emitDeviceCallMove(mlir::OpBuilder &b, DeviceHeapEmitter &heap,
                                      mlir::Value source, mlir::Value destination,
                                      unsigned cursorCell, std::span<const DeviceCallMove> moves) {
  using namespace mlir;
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t n) -> Value {
    return arith::ConstantIntOp::create(b, loc, n, 64);
  };
  const auto equal = [&](Value left, Value right) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, left, right);
  };
  const auto below = [&](Value left, Value right) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, left, right);
  };
  const auto add = [&](Value left, Value right) -> Value {
    return arith::AddIOp::create(b, loc, left, right);
  };
  const auto cursor = heap.load(cursorCell);
  Value sourceCell = c(0), destinationCell = c(0), outerSource = c(0), outerDestination = c(0);
  std::uint64_t count = 0;
  for (const auto &move : moves) {
    const auto sourceBase = move.outerSource == UINT32_MAX ? move.source : move.outerSource;
    const auto destinationBase = move.outerDestination == UINT32_MAX ? move.destination
                                                                      : move.outerDestination;
    if (!move.words || move.words > UINT32_MAX - count ||
        move.words - 1 > UINT32_MAX - move.source ||
        move.words - 1 > UINT32_MAX - move.destination ||
        move.words - 1 > UINT32_MAX - sourceBase ||
        move.words - 1 > UINT32_MAX - destinationBase)
      throw std::invalid_argument("invalid GPU call transfer extent");
    const Value selected = arith::AndIOp::create(b, loc, below(cursor, c(count + move.words)),
        arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, cursor, c(count)));
    const Value relative = arith::SubIOp::create(b, loc, cursor, c(count));
    sourceCell = arith::SelectOp::create(b, loc, selected, add(c(move.source), relative), sourceCell);
    destinationCell = arith::SelectOp::create(b, loc, selected, add(c(move.destination), relative),
                                            destinationCell);
    outerSource = arith::SelectOp::create(b, loc, selected, add(c(sourceBase), relative), outerSource);
    outerDestination = arith::SelectOp::create(b, loc, selected, add(c(destinationBase), relative),
                                             outerDestination);
    count += move.words;
  }
  const Value pc = heap.load(DeviceHeapEmitter::PC);
  const auto active = [&](Value state) -> Value {
    return arith::AndIOp::create(b, loc, equal(heap.load(DeviceHeapEmitter::PC), state),
        arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne, state, c(UINT64_MAX)));
  };
  const Value shouldCopy = arith::AndIOp::create(b, loc, active(pc), below(cursor, c(count)));
  auto copy = scf::IfOp::create(b, loc, shouldCopy, false);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&copy.getThenRegion().front());
    auto load = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, equal(source, c(0)), true);
    b.setInsertionPointToStart(&load.getThenRegion().front());
    scf::YieldOp::create(b, loc, heap.loadAt(outerSource));
    b.setInsertionPointToStart(&load.getElseRegion().front());
    scf::YieldOp::create(b, loc, heap.read(source, add(c(4), sourceCell)));
    b.setInsertionPointAfter(load);
    auto readable = scf::IfOp::create(b, loc, active(pc), false);
    b.setInsertionPointToStart(&readable.getThenRegion().front());
    auto write = scf::IfOp::create(b, loc, equal(destination, c(0)), true);
    b.setInsertionPointToStart(&write.getThenRegion().front());
    heap.storeAt(outerDestination, load.getResult(0));
    b.setInsertionPointToStart(&write.getElseRegion().front());
    heap.write(destination, add(c(4), destinationCell), load.getResult(0));
    b.setInsertionPointAfter(write);
    auto written = scf::IfOp::create(b, loc, active(pc), false);
    b.setInsertionPointToStart(&written.getThenRegion().front());
    heap.store(cursorCell, add(cursor, c(1)));
  }
  return arith::AndIOp::create(b, loc, active(pc), equal(heap.load(cursorCell), c(count)));
}
} // namespace kalandra
