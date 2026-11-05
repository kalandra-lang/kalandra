#include "DeviceHeap.h"
#include "../runtime/DeviceArrayABI.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include <set>
#include <stdexcept>
using namespace mlir;
namespace kalandra {
DeviceHeapEmitter::DeviceHeapEmitter(OpBuilder &builder, Value heap, Value frame, unsigned words,
                                     std::span<const DeviceRootMap> maps,
                                     std::span<const DeviceTraceLayout> traceLayouts,
                                     std::span<const DeviceArrayInput> arrayInputs)
    : b(builder), loc(b.getUnknownLoc()), arena(heap), params(frame), parameterWords(words),
      roots(maps), layouts(traceLayouts), inputs(arrayInputs) {
  for (const auto &input : inputs) {
    const auto buffer = mlir::dyn_cast<mlir::MemRefType>(input.buffer.getType());
    if (!buffer || buffer.getRank() != 1 || !buffer.getElementType().isInteger(64) ||
        !input.words.getType().isInteger(64) || !input.layout || input.layout > 11)
      throw std::invalid_argument("invalid external device array layout");
  }
  std::set<unsigned> identities;
  std::size_t fields = 0;
  for (const auto &layout : layouts) {
    if (layout.identity <= 11 || !identities.insert(layout.identity).second || !layout.stride ||
        layout.stride > runtime::detail::ControlArenaABI::MaxWords || layout.references.empty() ||
        layout.references.size() > 100000 - fields)
      throw std::invalid_argument("invalid device reference traversal layout");
    fields += layout.references.size();
    for (const auto &field : layout.references)
      if (field.cell >= layout.stride ||
          (field.tagCell != UINT32_MAX && field.tagCell >= layout.stride))
        throw std::invalid_argument("device reference field outside element layout");
  }
}
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
void DeviceHeapEmitter::storeAt(Value cell, Value value) {
  auto word = arith::MulIOp::create(b, loc, cell, c(2));
  auto index = [&](unsigned part) {
    return arith::IndexCastOp::create(b, loc, b.getIndexType(), add(word, c(part)));
  };
  auto low = arith::TruncIOp::create(b, loc, b.getI32Type(), value);
  auto high = arith::TruncIOp::create(b, loc, b.getI32Type(),
                                      arith::ShRUIOp::create(b, loc, value, c(32)));
  memref::StoreOp::create(b, loc, low, params, ValueRange{index(1)});
  memref::StoreOp::create(b, loc, high, params, ValueRange{index(2)});
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
Value DeviceHeapEmitter::readArena(Value h, Value field) {
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
void DeviceHeapEmitter::writeArena(Value h, Value field, Value v) {
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
Value DeviceHeapEmitter::read(Value h, Value field) {
  if (inputs.empty())
    return readArena(h, field);
  auto external = arith::CmpIOp::create(
      b, loc, arith::CmpIPredicate::ne,
      arith::AndIOp::create(b, loc, h, c(runtime::DeviceArrayWireABI::ExternalHandle)), c(0));
  auto branch = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, external, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  auto value = readInput(h, field);
  scf::YieldOp::create(b, loc, ValueRange{value});
  b.setInsertionPointToStart(&branch.getElseRegion().front());
  value = readArena(h, field);
  scf::YieldOp::create(b, loc, ValueRange{value});
  return branch.getResult(0);
}
void DeviceHeapEmitter::write(Value h, Value field, Value value) {
  if (inputs.empty()) {
    writeArena(h, field, value);
    return;
  }
  auto local =
      eq(arith::AndIOp::create(b, loc, h, c(runtime::DeviceArrayWireABI::ExternalHandle)), c(0));
  auto branch = scf::IfOp::create(b, loc, local, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  writeArena(h, field, value);
  b.setInsertionPointToStart(&branch.getElseRegion().front());
  fault(99); // Read-only input backing, including a forged write path.
}
void DeviceHeapEmitter::mark(Value h) {
  if (inputs.empty()) {
    markArena(h);
    return;
  }
  auto local =
      eq(arith::AndIOp::create(b, loc, h, c(runtime::DeviceArrayWireABI::ExternalHandle)), c(0));
  auto branch = scf::IfOp::create(b, loc, local, true);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  markArena(h);
  b.setInsertionPointToStart(&branch.getElseRegion().front());
  readInput(h, c(1)); // Validate; captured backing is retained outside the arena.
}
Value DeviceHeapEmitter::readInput(Value h, Value field) {
  Value result = c(0), found = arith::ConstantIntOp::create(b, loc, 0, 1);
  const auto both = [&](Value a, Value d) -> Value { return arith::AndIOp::create(b, loc, a, d); };
  const auto less = [&](Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ult, a, d);
  };
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &input = inputs[i];
    auto selected = eq(h, c(runtime::DeviceArrayWireABI::ExternalHandle | (i + 1)));
    found = arith::OrIOp::create(b, loc, found, selected);
    auto choose = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, selected, true);
    {
      OpBuilder::InsertionGuard guard(b);
      b.setInsertionPointToStart(&choose.getThenRegion().front());
      const auto load = [&](Value word) -> Value {
        auto index = arith::IndexCastOp::create(b, loc, b.getIndexType(), word);
        return memref::LoadOp::create(b, loc, input.buffer, ValueRange{index});
      };
      auto bounded = both(less(c(3), input.words), less(field, input.words));
      bounded = both(bounded, arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, input.words,
                                                    c(runtime::DeviceArrayWireABI::MaxWords)));
      auto access = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, bounded, true);
      {
        OpBuilder::InsertionGuard checked(b);
        b.setInsertionPointToStart(&access.getThenRegion().front());
        auto origin = load(c(3));
        auto valid = both(eq(load(c(0)), c(runtime::DeviceArrayWireABI::Magic)),
                          eq(load(c(1)), c(input.layout)));
        valid = both(valid, eq(load(c(2)), arith::SubIOp::create(b, loc, input.words, c(4))));
        valid = both(
            valid, both(less(c(0), origin), arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule,
                                                                  origin, c(UINT32_MAX))));
        auto typed = scf::IfOp::create(b, loc, TypeRange{b.getI64Type()}, valid, true);
        {
          OpBuilder::InsertionGuard canonical(b);
          b.setInsertionPointToStart(&typed.getThenRegion().front());
          auto value = load(field);
          // Present an ordinary read-only block descriptor to source array code.
          value = arith::SelectOp::create(b, loc, eq(field, c(0)), input.words, value);
          if (input.layout == 11) {
            auto payload = arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, field, c(4));
            auto invalid = both(payload, less(c(1), value));
            auto fail = scf::IfOp::create(b, loc, invalid, false);
            OpBuilder::InsertionGuard invalidGuard(b);
            b.setInsertionPointToStart(&fail.getThenRegion().front());
            fault(99);
          }
          scf::YieldOp::create(b, loc, ValueRange{value});
          b.setInsertionPointToStart(&typed.getElseRegion().front());
          fault(99);
          scf::YieldOp::create(b, loc, ValueRange{c(0)});
        }
        scf::YieldOp::create(b, loc, ValueRange{typed.getResult(0)});
        b.setInsertionPointToStart(&access.getElseRegion().front());
        fault(99);
        scf::YieldOp::create(b, loc, ValueRange{c(0)});
      }
      scf::YieldOp::create(b, loc, ValueRange{access.getResult(0)});
      b.setInsertionPointToStart(&choose.getElseRegion().front());
      scf::YieldOp::create(b, loc, ValueRange{result});
    }
    result = choose.getResult(0);
  }
  auto missing = scf::IfOp::create(
      b, loc, arith::XOrIOp::create(b, loc, found, arith::ConstantIntOp::create(b, loc, 1, 1)),
      false);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&missing.getThenRegion().front());
    fault(99);
  }
  return result;
}
// Header mark word: zero is white, one black, (next << 2) | 2 grey.
// Handles are bounded to UINT32_MAX words by ControlArenaABI, so links fit.
// Only the paused mutator's live roots and typed payloads may supply handles.
void DeviceHeapEmitter::markArena(Value h) {
  auto branch = scf::IfOp::create(
      b, loc, arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne, h, c(0)), false);
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(&branch.getThenRegion().front());
  auto layout = read(h, c(1));
  Value known = arith::AndIOp::create(
      b, loc, arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, layout, c(1)),
      arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, layout, c(11)));
  Value owned = arith::ConstantIntOp::create(b, loc, 0, 1);
  for (const auto &descriptor : layouts)
    owned = arith::OrIOp::create(b, loc, owned, eq(layout, c(descriptor.identity)));
  known = arith::OrIOp::create(b, loc, known, owned);
  auto span = read(h, c(0));
  auto bounded = arith::AndIOp::create(
      b, loc, arith::CmpIOp::create(b, loc, arith::CmpIPredicate::uge, span, c(4)),
      arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, span,
                            arith::SubIOp::create(b, loc, load(Capacity), h)));
  auto payload = arith::SubIOp::create(b, loc, span, c(4));
  bounded = arith::AndIOp::create(
      b, loc, bounded,
      arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ule, read(h, c(2)), payload));
  auto valid = scf::IfOp::create(b, loc, arith::AndIOp::create(b, loc, known, bounded), true);
  b.setInsertionPointToStart(&valid.getThenRegion().front());
  auto white = scf::IfOp::create(b, loc, eq(read(h, c(3)), c(0)), false);
  {
    OpBuilder::InsertionGuard marking(b);
    b.setInsertionPointToStart(&white.getThenRegion().front());
    auto next =
        arith::OrIOp::create(b, loc, arith::ShLIOp::create(b, loc, load(GreyHead), c(2)), c(2));
    write(h, c(3), arith::SelectOp::create(b, loc, owned, next, c(1)));
    store(GreyHead, arith::SelectOp::create(b, loc, owned, h, load(GreyHead)));
  }
  b.setInsertionPointToStart(&valid.getElseRegion().front());
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
  for (unsigned state : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u})
    validPhase = arith::OrIOp::create(b, loc, validPhase, eq(phase, c(state)));
  when(arith::XOrIOp::create(b, loc, validPhase, arith::ConstantIntOp::create(b, loc, 1, 1)),
       [&] { fault(99); });
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
          store(GreyHead, c(0));
          store(TraceHandle, c(0));
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
              mark(handle);
              store(RootCursor, add(root, c(1)));
            },
            [&] {
              store(Phase, c(layouts.empty() ? 5 : 4));
              store(Cursor, c(1));
              store(PreviousFree, c(0));
            });
      });
    }
    when(arith::XOrIOp::create(b, loc, matched, arith::ConstantIntOp::create(b, loc, 1, 1)),
         [&] { fault(99); });
  });
  // One queue pop or typed reference field per step. No auxiliary allocation,
  // recursive traversal or repeated full-heap scans, even for cycles.
  when(eq(phase, c(4)), [&] {
    conditional(
        eq(load(TraceHandle), c(0)),
        [&] {
          auto h = load(GreyHead);
          conditional(
              eq(h, c(0)),
              [&] {
                store(Phase, c(5));
                store(Cursor, c(1));
                store(PreviousFree, c(0));
              },
              [&] {
                auto state = read(h, c(3));
                conditional(
                    eq(arith::AndIOp::create(b, loc, state, c(3)), c(2)),
                    [&] {
                      store(GreyHead, arith::ShRUIOp::create(b, loc, state, c(2)));
                      write(h, c(3), c(1)); // Black before visiting self/cyclic edges.
                      store(TraceHandle, h);
                      store(TraceElement, c(0));
                      store(TraceField, c(0));
                    },
                    [&] { fault(99); });
              });
        },
        [&] {
          auto h = load(TraceHandle), layout = read(h, c(1)), length = read(h, c(2));
          Value found = arith::ConstantIntOp::create(b, loc, 0, 1);
          for (const auto &descriptor : layouts) {
            auto selected = eq(layout, c(descriptor.identity));
            found = arith::OrIOp::create(b, loc, found, selected);
            when(selected, [&] {
              auto element = load(TraceElement), field = load(TraceField);
              auto valid = eq(arith::RemUIOp::create(b, loc, length, c(descriptor.stride)), c(0));
              conditional(
                  valid,
                  [&] {
                    conditional(
                        compare(arith::CmpIPredicate::ult, element, length),
                        [&] {
                          Value offset = c(0), tagCell = c(0), tag = c(0);
                          Value guarded = arith::ConstantIntOp::create(b, loc, 0, 1);
                          for (std::size_t i = 0; i < descriptor.references.size(); ++i) {
                            const auto &reference = descriptor.references[i];
                            auto chosen = eq(field, c(i));
                            offset =
                                arith::SelectOp::create(b, loc, chosen, c(reference.cell), offset);
                            if (reference.tagCell != UINT32_MAX) {
                              tagCell = arith::SelectOp::create(b, loc, chosen,
                                                                c(reference.tagCell), tagCell);
                              tag = arith::SelectOp::create(b, loc, chosen, c(reference.tag), tag);
                              guarded = arith::OrIOp::create(b, loc, guarded, chosen);
                            }
                          }
                          conditional(
                              compare(arith::CmpIPredicate::ult, field,
                                      c(descriptor.references.size())),
                              [&] {
                                auto active = arith::OrIOp::create(
                                    b, loc,
                                    arith::XOrIOp::create(
                                        b, loc, guarded,
                                        arith::ConstantIntOp::create(b, loc, 1, 1)),
                                    eq(read(h, add(c(4), add(element, tagCell))), tag));
                                when(active,
                                     [&] { mark(read(h, add(c(4), add(element, offset)))); });
                                auto next = add(field, c(1));
                                auto done = eq(next, c(descriptor.references.size()));
                                store(TraceField,
                                      arith::SelectOp::create(b, loc, done, c(0), next));
                                store(TraceElement,
                                      arith::SelectOp::create(b, loc, done,
                                                              add(element, c(descriptor.stride)),
                                                              element));
                              },
                              [&] { fault(99); });
                        },
                        [&] { store(TraceHandle, c(0)); });
                  },
                  [&] { fault(99); });
            });
          }
          when(arith::XOrIOp::create(b, loc, found, arith::ConstantIntOp::create(b, loc, 1, 1)),
               [&] { fault(99); });
        });
  });
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
