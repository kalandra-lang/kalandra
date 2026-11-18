#pragma once
#include "DeviceHeap.h"
#include "kalandra/GpuTextWire.h"
#include "kalandra/Semantics.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"

namespace kalandra {
// A string handle points to the canonical UTF-8 word packet in the control
// arena, or to the same packet carried by a read-only external array input.
// One call emits one bounded step. Local cursor state survives arena growth.
class DeviceTextControl {
  using Value = mlir::Value;
  using Pred = mlir::arith::CmpIPredicate;
  enum Cell { Cursor, Count, Begin, End, Phase };
  mlir::OpBuilder &b;
  mlir::Location loc;
  DeviceHeapEmitter &heap;
  unsigned base;

  Value c(std::uint64_t value) { return mlir::arith::ConstantIntOp::create(b, loc, value, 64); }
  Value test(Pred predicate, Value a, Value d) {
    return mlir::arith::CmpIOp::create(b, loc, predicate, a, d);
  }
  Value eq(Value a, Value d) { return test(Pred::eq, a, d); }
  Value lt(Value a, Value d) { return test(Pred::ult, a, d); }
  Value both(Value a, Value d) { return mlir::arith::AndIOp::create(b, loc, a, d); }
  Value add(Value a, Value d) { return mlir::arith::AddIOp::create(b, loc, a, d); }
  Value sub(Value a, Value d) { return mlir::arith::SubIOp::create(b, loc, a, d); }
  Value choose(Value predicate, Value a, Value d) {
    return mlir::arith::SelectOp::create(b, loc, predicate, a, d);
  }
  Value get(Cell cell) { return heap.load(base + cell); }
  void set(Cell cell, Value value) { heap.store(base + cell, value); }
  Value packet(Value handle, Value position) { return heap.read(handle, add(c(4), position)); }
  template <class Body> void when(Value predicate, Body emit) {
    auto branch = mlir::scf::IfOp::create(b, loc, predicate, false);
    mlir::OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&branch.getThenRegion().front());
    emit();
  }
  template <class Yes, class No> Value branch(Value predicate, Yes yes, No no) {
    auto op = mlir::scf::IfOp::create(b, loc, mlir::TypeRange{b.getI64Type()}, predicate, true);
    mlir::OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&op.getThenRegion().front());
    mlir::scf::YieldOp::create(b, loc, mlir::ValueRange{yes()});
    b.setInsertionPointToStart(&op.getElseRegion().front());
    mlir::scf::YieldOp::create(b, loc, mlir::ValueRange{no()});
    return op.getResult(0);
  }
  Value byte(Value handle, Value firstWord, Value position) {
    auto offset = mlir::arith::ShRUIOp::create(b, loc, position, c(2));
    auto shift = mlir::arith::ShLIOp::create(b, loc, both(position, c(3)), c(3));
    return both(mlir::arith::ShRUIOp::create(b, loc, packet(handle, add(firstWord, offset)), shift),
                c(255));
  }
  Value scalarWidth(Value first) {
    return choose(lt(first, c(0x80)), c(1),
                  choose(lt(first, c(0xe0)), c(2), choose(lt(first, c(0xf0)), c(3), c(4))));
  }
  void reset() {
    for (auto cell : {Cursor, Count, Begin, End, Phase})
      set(cell, c(0));
  }
  template <class Read, class Publish> Value build(Value bytes, Read byteAt, Publish publish) {
    using W = runtime::DeviceArrayWireABI;
    const auto start = GpuTextWire::payloadStart(1);
    const auto words = add(c(start), mlir::arith::ShRUIOp::create(b, loc, add(bytes, c(3)), c(2)));
    const auto valid = test(Pred::ule, bytes, c(UINT32_MAX));
    when(test(Pred::ugt, bytes, c(UINT32_MAX)), [&] { heap.fault(3); });
    auto ready = heap.ready(), at = heap.cursor();
    auto complete = both(ready, eq(at, words));
    when(both(valid, eq(heap.load(DeviceHeapEmitter::Phase), c(0))),
         [&] { heap.begin(words, W::layout(NumericType::UInt32, false)); });
    when(both(ready, lt(at, words)), [&] {
      auto word = branch(
          lt(at, c(start)),
          [&] { return choose(eq(at, c(0)), c(1), choose(eq(at, c(2)), c(0), bytes)); },
          [&] {
            auto position = mlir::arith::ShLIOp::create(b, loc, sub(at, c(start)), c(2));
            Value payload = c(0);
            for (unsigned i = 0; i < 4; ++i) {
              auto offset = add(position, c(i));
              auto item =
                  branch(lt(offset, bytes), [&] { return byteAt(offset); }, [&] { return c(0); });
              payload = mlir::arith::OrIOp::create(
                  b, loc, payload, mlir::arith::ShLIOp::create(b, loc, item, c(i * 8)));
            }
            return payload;
          });
      heap.write(heap.handle(), add(c(4), at), word);
      heap.advance();
    });
    when(complete, [&] {
      publish(heap.handle());
      heap.finish();
      reset();
    });
    return complete;
  }

public:
  static constexpr unsigned Cells = Phase + 1;
  DeviceTextControl(mlir::OpBuilder &builder, DeviceHeapEmitter &arena, unsigned frameBase)
      : b(builder), loc(b.getUnknownLoc()), heap(arena), base(frameBase) {}

  Value byteLength(Value handle) { return packet(handle, c(1)); }
  Value batchLength(Value handle) { return packet(handle, c(0)); }
  template <class Publish> Value length(Value handle, Publish publish) {
    auto cursor = get(Cursor), count = get(Count);
    auto done = eq(cursor, byteLength(handle));
    when(done, [&] {
      publish(count);
      reset();
    });
    when(lt(cursor, byteLength(handle)), [&] {
      set(Cursor, add(cursor, scalarWidth(byte(handle, c(GpuTextWire::payloadStart(1)), cursor))));
      set(Count, add(count, c(1)));
    });
    return done;
  }
  template <class Publish> Value equal(Value left, Value right, bool invert, Publish publish) {
    const auto cursor = get(Cursor), bytes = byteLength(left);
    const auto lengthMatches = eq(bytes, byteLength(right));
    const auto compare = both(lengthMatches, lt(cursor, bytes));
    auto comparison = branch(
        compare,
        [&] {
          auto firstWord = c(GpuTextWire::payloadStart(1));
          return choose(eq(byte(left, firstWord, cursor), byte(right, firstWord, cursor)), c(1),
                        c(0));
        },
        [&] { return choose(lengthMatches, c(1), c(0)); });
    auto more = both(compare, eq(comparison, c(1)));
    when(more, [&] { set(Cursor, add(cursor, c(1))); });
    auto done = eq(choose(more, c(1), c(0)), c(0));
    when(done, [&] {
      publish(invert ? sub(c(1), comparison) : comparison);
      reset();
    });
    return done;
  }
  template <class Publish> Value concat(Value left, Value right, Publish publish) {
    auto firstWord = c(GpuTextWire::payloadStart(1)), size = byteLength(left);
    return build(
        add(size, byteLength(right)),
        [&](Value offset) {
          return branch(
              lt(offset, size), [&] { return byte(left, firstWord, offset); },
              [&] { return byte(right, firstWord, sub(offset, size)); });
        },
        publish);
  }
  template <class Publish>
  Value slice(Value handle, Value start, Value end, bool index, Publish publish) {
    const auto firstWord = c(GpuTextWire::payloadStart(1));
    const auto phase = get(Phase), count = get(Count), cursor = get(Cursor),
               size = byteLength(handle);
    const auto copying = eq(phase, c(1));
    const auto validRange = both(test(Pred::ule, start, end), test(Pred::ule, end, size));
    when(eq(choose(validRange, c(1), c(0)), c(0)), [&] { heap.fault(1); });
    when(both(validRange, eq(phase, c(0))), [&] {
      when(eq(count, start), [&] { set(Begin, cursor); });
      when(eq(count, end), [&] {
        set(End, cursor);
        set(Phase, c(1));
      });
      when(lt(count, end), [&] {
        when(eq(cursor, size), [&] { heap.fault(1); });
        when(lt(cursor, size), [&] {
          set(Cursor, add(cursor, scalarWidth(byte(handle, firstWord, cursor))));
          set(Count, add(count, c(1)));
        });
      });
    });
    // The index end is formed by the caller. Guard wrap independently of the
    // half-open slice check.
    if (index)
      when(eq(end, c(0)), [&] { heap.fault(1); });
    return eq(branch(
                  copying,
                  [&] {
                    auto begin = get(Begin), finish = get(End);
                    return choose(build(
                                      sub(finish, begin),
                                      [&](Value offset) {
                                        return byte(handle, firstWord, add(begin, offset));
                                      },
                                      publish),
                                  c(1), c(0));
                  },
                  [&] { return c(0); }),
              c(1));
  }
  template <class Publish> Value row(Value batch, Value index, Publish publish) {
    const auto rows = batchLength(batch);
    auto valid = lt(index, rows);
    when(test(Pred::uge, index, rows), [&] { heap.fault(1); });
    return eq(branch(
                  valid,
                  [&] {
                    const auto begin = packet(batch, add(c(GpuTextWire::HeaderWords), index));
                    const auto end = packet(batch, add(c(GpuTextWire::HeaderWords + 1), index));
                    const auto firstWord = add(c(GpuTextWire::HeaderWords + 1), rows);
                    return choose(build(
                                      sub(end, begin),
                                      [&](Value offset) {
                                        return byte(batch, firstWord, add(begin, offset));
                                      },
                                      publish),
                                  c(1), c(0));
                  },
                  [&] { return c(0); }),
              c(1));
  }
  template <class Publish> Value literal(std::string_view text, Publish publish) {
    return build(
        c(text.size()),
        [&](Value offset) {
          Value data = c(0);
          for (std::size_t i = 0; i < text.size(); ++i)
            data = choose(eq(offset, c(i)), c(static_cast<unsigned char>(text[i])), data);
          return data;
        },
        publish);
  }
};
} // namespace kalandra
