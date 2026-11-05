#pragma once
#include "DeviceCallTransfer.h"

namespace kalandra {
struct DeviceCallReturn {
  unsigned frameCell, cursorCell, outerSuspensionCell, layout, words;
  std::uint64_t returnPC;
  DeviceCallMove result;
  unsigned rootWords, parentLayout, parentWords;
};

// The return dispatcher selects a known caller site before entering this
// step. Do not restore its PC or unlink the callee until its result is copied.
inline void emitDeviceCallReturn(mlir::OpBuilder &b, DeviceHeapEmitter &heap,
                                 const DeviceCallReturn &site) {
  using namespace mlir;
  const auto fits = [&](unsigned offset, unsigned words) {
    return offset < words && site.result.words <= words - offset;
  };
  const auto outerSlot = site.result.outerDestination == UINT32_MAX ? site.result.destination
                                                                      : site.result.outerDestination;
  if ((!site.rootWords && !site.parentLayout) ||
      (site.rootWords && !fits(outerSlot, site.rootWords)) ||
      (site.parentLayout && (site.parentLayout <= 11 || site.result.destination < 4 ||
                             !fits(site.result.destination, site.parentWords))) ||
      site.layout <= 11 || site.result.source < 4 || site.result.source >= site.words ||
      site.result.words > site.words - site.result.source || !site.returnPC)
    throw std::invalid_argument("invalid GPU call return extent");
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t n) -> Value {
    return arith::ConstantIntOp::create(b, loc, n, 64);
  };
  const auto equal = [&](Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
  };
  const auto both = [&](Value a, Value d) -> Value {
    return arith::AndIOp::create(b, loc, a, d);
  };
  const auto frame = heap.load(site.frameCell);
  const auto pc = heap.load(DeviceHeapEmitter::PC);
  const auto parent = heap.read(frame, c(4));
  const auto valid = both(equal(heap.read(frame, c(1)), c(site.layout)),
      both(equal(heap.read(frame, c(2)), c(site.words)),
           both(equal(heap.read(frame, c(5)), c(site.returnPC)),
                equal(heap.read(frame, c(6)), c(site.result.destination)))));
  auto parentChecked = scf::IfOp::create(b, loc, TypeRange{b.getI1Type()},
                                         equal(parent, c(0)), true);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&parentChecked.getThenRegion().front());
    const Value rootAllowed = site.rootWords
        ? equal(heap.load(site.outerSuspensionCell), c(site.returnPC))
        : Value(arith::ConstantIntOp::create(b, loc, false, 1));
    scf::YieldOp::create(b, loc, rootAllowed);
    b.setInsertionPointToStart(&parentChecked.getElseRegion().front());
    Value validParent = arith::ConstantIntOp::create(b, loc, false, 1);
    if (site.parentLayout) {
      const Value distinct = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne,
                                                    parent, frame);
      validParent = both(distinct, both(equal(heap.read(parent, c(1)), c(site.parentLayout)),
                                      both(equal(heap.read(parent, c(2)), c(site.parentWords)),
                                           equal(heap.read(parent, c(7)), c(site.returnPC)))));
    }
    scf::YieldOp::create(b, loc, validParent);
  }
  const Value safe = both(both(valid, parentChecked.getResult(0)),
                           equal(heap.load(DeviceHeapEmitter::PC), pc));
  auto checked = scf::IfOp::create(b, loc, safe, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&checked.getThenRegion().front());
  const auto done = emitDeviceCallMove(b, heap, frame, parent, site.cursorCell,
                                       std::span(&site.result, 1));
  auto publish = scf::IfOp::create(b, loc, done, false);
  b.setInsertionPointToStart(&publish.getThenRegion().front());
  auto outer = scf::IfOp::create(b, loc, equal(parent, c(0)), true);
  {
    OpBuilder::InsertionGuard parentGuard(b);
    b.setInsertionPointToStart(&outer.getThenRegion().front());
    heap.store(site.outerSuspensionCell, c(0));
    b.setInsertionPointToStart(&outer.getElseRegion().front());
    heap.write(parent, c(7), c(0));
  }
  auto restore = scf::IfOp::create(b, loc, equal(heap.load(DeviceHeapEmitter::PC), pc), false);
  b.setInsertionPointToStart(&restore.getThenRegion().front());
  heap.store(site.frameCell, parent);
  heap.store(site.cursorCell, c(0));
  heap.store(DeviceHeapEmitter::PC, c(site.returnPC));
  b.setInsertionPointToStart(&checked.getElseRegion().front());
  heap.fault(99);
}
} // namespace kalandra
