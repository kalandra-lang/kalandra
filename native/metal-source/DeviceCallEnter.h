#pragma once
#include "DeviceCallTransfer.h"

namespace kalandra {
struct DeviceCallEntry {
  unsigned frameCell, cursorCell, outerSuspensionCell, layout, words;
  std::uint64_t entryPC, returnPC, returnSlot;
  std::span<const DeviceCallMove> arguments;
};

// Called at a known call PC after allocator service allows source work.
// Until all arguments are written, the suspended frame remains unpublished.
inline void emitDeviceCallEnter(mlir::OpBuilder &b, DeviceHeapEmitter &heap,
                                const DeviceCallEntry &entry) {
  using namespace mlir;
  constexpr unsigned header = 4, metadata = 4;
  if (entry.words < metadata || !entry.entryPC || !entry.returnPC || entry.layout <= 11)
    throw std::invalid_argument("invalid GPU call frame entry");
  for (const auto &move : entry.arguments)
    if (move.destination < metadata || move.destination >= entry.words ||
        move.words > entry.words - move.destination)
      throw std::invalid_argument("GPU call argument lies outside the callee frame");
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t n) -> Value {
    return arith::ConstantIntOp::create(b, loc, n, 64);
  };
  const auto equal = [&](Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
  };
  const auto when = [&](Value condition, auto body) {
    auto branch = scf::IfOp::create(b, loc, condition, false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    body();
  };
  auto phase = scf::IfOp::create(b, loc, heap.ready(), true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&phase.getThenRegion().front());
  const auto frame = heap.handle();
  const auto caller = heap.load(entry.frameCell);
  const auto pc = heap.load(DeviceHeapEmitter::PC);
  when(equal(heap.load(entry.cursorCell), c(0)), [&] {
    heap.write(frame, c(header), caller);
    heap.write(frame, c(header + 1), c(entry.returnPC));
    heap.write(frame, c(header + 2), c(entry.returnSlot));
    heap.write(frame, c(header + 3), c(0));
  });
  when(equal(heap.load(DeviceHeapEmitter::PC), pc), [&] {
    when(emitDeviceCallMove(b, heap, caller, frame, entry.cursorCell, entry.arguments), [&] {
      auto outer = scf::IfOp::create(b, loc, equal(caller, c(0)), true);
      {
        OpBuilder::InsertionGuard callerGuard(b);
        b.setInsertionPointToStart(&outer.getThenRegion().front());
        heap.store(entry.outerSuspensionCell, c(entry.returnPC));
        b.setInsertionPointToStart(&outer.getElseRegion().front());
        heap.write(caller, c(header + 3), c(entry.returnPC));
      }
      when(equal(heap.load(DeviceHeapEmitter::PC), pc), [&] {
        heap.store(entry.frameCell, frame);
        heap.store(entry.cursorCell, c(0));
        heap.finish();
        heap.store(DeviceHeapEmitter::PC, c(entry.entryPC));
      });
    });
  });
  b.setInsertionPointToStart(&phase.getElseRegion().front());
  heap.store(entry.cursorCell, c(0));
  heap.begin(c(entry.words), entry.layout);
}
} // namespace kalandra
