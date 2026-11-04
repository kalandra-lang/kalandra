#include "DeviceHeap.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
using namespace mlir;
namespace kalandra {
DeviceHeapEmitter::DeviceHeapEmitter(OpBuilder &builder, Value heap, Value frame, unsigned words,
                                     std::span<const DeviceRootMap> maps)
    : b(builder), loc(b.getUnknownLoc()), arena(heap), params(frame), parameterWords(words),
      roots(maps) {}
Value DeviceHeapEmitter::c(std::uint64_t n) {
  return arith::ConstantOp::create(b, loc, b.getI64Type(), b.getI64IntegerAttr(n));
}
Value DeviceHeapEmitter::add(Value a, Value d) { return arith::AddIOp::create(b, loc, a, d); }
Value DeviceHeapEmitter::eq(Value a, Value d) {
  return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
}
Value DeviceHeapEmitter::loadAt(Value cell) {
  auto index = [&](unsigned part) {
    auto word = add(c(part), arith::MulIOp::create(b, loc, cell, c(2)));
    return arith::IndexCastOp::create(b, loc, b.getIndexType(), word);
  };
  auto low = memref::LoadOp::create(b, loc, params, ValueRange{index(1)});
  auto high = memref::LoadOp::create(b, loc, params, ValueRange{index(2)});
  return arith::OrIOp::create(
      b, loc, arith::ExtUIOp::create(b, loc, b.getI64Type(), low),
      arith::ShLIOp::create(b, loc, arith::ExtUIOp::create(b, loc, b.getI64Type(), high), c(32)));
}
Value DeviceHeapEmitter::load(unsigned cell) { return loadAt(c(cell)); }
void DeviceHeapEmitter::store(unsigned cell, Value v) {
  auto at = [&](unsigned n) { return arith::ConstantIndexOp::create(b, loc, n); };
  auto lo = arith::TruncIOp::create(b, loc, b.getI32Type(), v);
  auto hi =
      arith::TruncIOp::create(b, loc, b.getI32Type(), arith::ShRUIOp::create(b, loc, v, c(32)));
  memref::StoreOp::create(b, loc, lo, params, ValueRange{at(1 + 2 * cell)});
  memref::StoreOp::create(b, loc, hi, params, ValueRange{at(2 + 2 * cell)});
}
void DeviceHeapEmitter::fault(unsigned code) {
  auto at = arith::ConstantIndexOp::create(b, loc, parameterWords - 1);
  auto flag = memref::LoadOp::create(b, loc, params, ValueRange{at});
  auto none = arith::ConstantIntOp::create(b, loc, UINT32_MAX, 32);
  auto first = scf::IfOp::create(b, loc, eq(flag, none), false);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&first.getThenRegion().front());
  memref::StoreOp::create(b, loc, arith::ConstantIntOp::create(b, loc, code, 32), params,
                          ValueRange{at});
  store(PC, c(UINT64_MAX));
}
Value DeviceHeapEmitter::read(Value h, Value field) {
  auto less = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, h, load(Capacity));
  auto rest = arith::SubIOp::create(b, loc, load(Capacity), h);
  auto bounded = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, field, rest);
  auto valid = arith::AndIOp::create(b, loc, less, bounded);
  auto choose = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, valid, true);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&choose.getThenRegion().front());
    auto index = arith::IndexCastOp::create(b, loc, b.getIndexType(), add(h, field));
    auto value = memref::LoadOp::create(b, loc, arena, ValueRange{index});
    scf::YieldOp::create(b, loc, ValueRange{value});
    b.setInsertionPointToStart(&choose.getElseRegion().front());
    fault(99); // Invalid private metadata, distinct from source Bounds.
    scf::YieldOp::create(b, loc, ValueRange{c(0)});
  }
  return choose.getResult(0);
}
void DeviceHeapEmitter::write(Value h, Value field, Value v) {
  auto less = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, h, load(Capacity));
  auto rest = arith::SubIOp::create(b, loc, load(Capacity), h);
  auto bounded = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, field, rest);
  auto choose = scf::IfOp::create(b, loc, arith::AndIOp::create(b, loc, less, bounded), true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&choose.getThenRegion().front());
  auto index = arith::IndexCastOp::create(b, loc, b.getIndexType(), add(h, field));
  memref::StoreOp::create(b, loc, v, arena, ValueRange{index});
  b.setInsertionPointToStart(&choose.getElseRegion().front());
  fault(99);
}
void DeviceHeapEmitter::initialize() {
  write(c(1), c(0), arith::SubIOp::create(b, loc, load(Capacity), c(1)));
  write(c(1), c(1), c(0));
  write(c(1), c(2), c(0));
  write(c(1), c(3), c(0));
}
void DeviceHeapEmitter::begin(Value length, unsigned layout) {
  auto valid = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, length,
                                     c(runtime::detail::ControlArenaABI::MaxWords - 4));
  auto branch = scf::IfOp::create(b, loc, valid, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  store(Need, add(length, c(4)));
  store(Length, length);
  store(Layout, c(layout));
  store(Cursor, c(1));
  store(Collected, c(0));
  store(Phase, c(1));
  b.setInsertionPointToStart(&branch.getElseRegion().front());
  fault(3); // Budget, before any overflowing address arithmetic.
}
Value DeviceHeapEmitter::ready() { return eq(load(Phase), c(8)); }
Value DeviceHeapEmitter::handle() { return load(Allocation); }
Value DeviceHeapEmitter::cursor() { return load(Cursor); }
void DeviceHeapEmitter::advance() { store(Cursor, add(load(Cursor), c(1))); }
void DeviceHeapEmitter::finish() { store(Phase, c(0)); }
Value DeviceHeapEmitter::service() {
  auto phase = load(Phase);
  auto compare = [&](arith::CmpIPredicate pred, Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, pred, a, d);
  };
  auto conditional = [&](Value condition, auto then, auto otherwise) {
    auto branch = scf::IfOp::create(b, loc, condition, true);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    then();
    b.setInsertionPointToStart(&branch.getElseRegion().front());
    otherwise();
  };
  auto when = [&](Value condition, auto body) {
    auto branch = scf::IfOp::create(b, loc, condition, false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    body();
  };
  Value validPhase = arith::ConstantIntOp::create(b, loc, 0, 1);
  for (unsigned state : {0u, 1u, 2u, 3u, 5u, 6u, 8u})
    validPhase = arith::OrIOp::create(b, loc, validPhase, eq(phase, c(state)));
  when(arith::XOrIOp::create(b, loc, validPhase,
           arith::ConstantIntOp::create(b, loc, 1, 1)), [&] { fault(99); });
  // One physical block per scan step. Never read or skip a malformed span.
  auto block = [&](auto body) {
    auto h = load(Cursor);
    auto span = read(h, c(0));
    auto minimum = compare(arith::CmpIPredicate::uge, span, c(4));
    auto available = arith::SubIOp::create(b, loc, load(Capacity), h);
    auto maximum = compare(arith::CmpIPredicate::ule, span, available);
    conditional(
        arith::AndIOp::create(b, loc, minimum, maximum), [&] { body(h, span); },
        [&] { fault(99); });
  };
  when(eq(phase, c(1)), [&] {
    conditional(
        compare(arith::CmpIPredicate::ult, load(Cursor), load(Capacity)),
        [&] {
          block([&](Value h, Value span) {
            auto free = eq(read(h, c(1)), c(0));
            auto fits = compare(arith::CmpIPredicate::uge, span, load(Need));
            conditional(
                arith::AndIOp::create(b, loc, free, fits),
                [&] {
                  auto remainder = arith::SubIOp::create(b, loc, span, load(Need));
                  auto split = compare(arith::CmpIPredicate::uge, remainder, c(4));
                  auto size = arith::SelectOp::create(b, loc, split, load(Need), span);
                  when(split, [&] {
                    auto tail = add(h, load(Need));
                    write(tail, c(0), remainder);
                    write(tail, c(1), c(0));
                    write(tail, c(2), c(0));
                    write(tail, c(3), c(0));
                  });
                  write(h, c(0), size);
                  write(h, c(1), load(Layout));
                  write(h, c(2), load(Length));
                  write(h, c(3), c(1));
                  store(Allocation, h);
                  store(Cursor, c(0));
                  store(Phase, c(8));
                },
                [&] { store(Cursor, add(h, span)); });
          });
        },
        [&] {
          conditional(
              eq(load(Collected), c(0)),
              [&] {
                store(Phase, c(2));
                store(Cursor, c(1));
              },
              [&] {
                auto capacity = load(Capacity), need = load(Need);
                auto possible =
                    compare(arith::CmpIPredicate::ule, need,
                            arith::SubIOp::create(
                                b, loc, c(runtime::detail::ControlArenaABI::MaxWords), capacity));
                conditional(
                    possible,
                    [&] {
                      auto doubled = add(capacity, capacity);
                      auto minimum = add(capacity, need);
                      auto next = arith::MaxUIOp::create(b, loc, doubled, minimum);
                      store(Requested,
                            arith::MinUIOp::create(b, loc, next,
                                                   c(runtime::detail::ControlArenaABI::MaxWords)));
                      store(Resume, load(PC));
                      store(GrowStart, capacity);
                      store(Phase, c(6));
                      store(PC, c(UINT64_MAX - 1));
                    },
                    [&] { fault(3); });
              });
        });
  });
  when(eq(phase, c(2)), [&] {
    conditional(
        compare(arith::CmpIPredicate::ult, load(Cursor), load(Capacity)),
        [&] {
          block([&](Value h, Value span) {
            write(h, c(3), c(0));
            store(Cursor, add(h, span));
          });
        },
        [&] {
          store(Phase, c(3));
          store(RootCursor, c(0));
        });
  });
  when(eq(phase, c(3)), [&] {
    auto pc = load(PC);
    Value matched = arith::ConstantIntOp::create(b, loc, 0, 1);
    for (std::size_t i = 0; i < roots.size(); ++i) {
      auto selected = eq(pc, c(roots[i].pc));
      matched = arith::OrIOp::create(b, loc, matched, selected);
      when(selected, [&] {
        auto root = load(RootCursor);
        conditional(
            compare(arith::CmpIPredicate::ult, root, c(roots[i].cells.size())),
            [&] {
              Value offset = c(0);
              for (std::size_t j = 0; j < roots[i].cells.size(); ++j)
                offset =
                    arith::SelectOp::create(b, loc, eq(root, c(j)), c(roots[i].cells[j]), offset);
              auto handle = loadAt(offset);
              when(compare(arith::CmpIPredicate::ne, handle, c(0)), [&] {
                auto layout = read(handle, c(1));
                auto valid =
                    arith::AndIOp::create(b, loc, compare(arith::CmpIPredicate::uge, layout, c(1)),
                                          compare(arith::CmpIPredicate::ule, layout, c(11)));
                conditional(valid, [&] { write(handle, c(3), c(1)); }, [&] { fault(99); });
              });
              store(RootCursor, add(root, c(1)));
            },
            [&] {
              store(Phase, c(5));
              store(Cursor, c(1));
              store(PreviousFree, c(0));
            });
      });
    }
    when(arith::XOrIOp::create(b, loc, matched, arith::ConstantIntOp::create(b, loc, 1, 1)),
         [&] { fault(99); });
  });
  // Initial layouts are scalar numeric/Bool arrays: they contain no references
  // to trace. Owned payload layouts will require an additional bounded grey scan.
  when(eq(phase, c(5)), [&] {
    conditional(
        compare(arith::CmpIPredicate::ult, load(Cursor), load(Capacity)),
        [&] {
          block([&](Value h, Value span) {
            auto unmarked = eq(read(h, c(3)), c(0));
            when(unmarked, [&] { write(h, c(1), c(0)); });
            conditional(
                eq(read(h, c(1)), c(0)),
                [&] {
                  auto previous = load(PreviousFree);
                  conditional(
                      compare(arith::CmpIPredicate::ne, previous, c(0)),
                      [&] { write(previous, c(0), add(read(previous, c(0)), span)); },
                      [&] { store(PreviousFree, h); });
                },
                [&] { store(PreviousFree, c(0)); });
            store(Cursor, add(h, span));
          });
        },
        [&] {
          store(Collected, c(1));
          store(Phase, c(1));
          store(Cursor, c(1));
        });
  });
  when(eq(phase, c(6)), [&] {
    auto h = load(GrowStart);
    write(h, c(0), arith::SubIOp::create(b, loc, load(Capacity), h));
    write(h, c(1), c(0));
    write(h, c(2), c(0));
    write(h, c(3), c(0));
    store(Phase, c(1));
    store(Cursor, c(1));
  });
  return arith::OrIOp::create(b, loc, eq(phase, c(0)), eq(phase, c(8)));
}
} // namespace kalandra
