#include "DeviceArrayReturn.h"
#include "../gpu-runtime/ControlExport.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
using namespace mlir;
namespace kalandra {
void emitDeviceArrayReturn(DeviceHeapEmitter &heap, OpBuilder &b, Value root, NumericType element,
                           bool boolean) {
  using A = runtime::detail::ControlArrayExportABI;
  using W = runtime::DeviceArrayWireABI;
  static_assert(A::Root == DeviceHeapEmitter::Cells);
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t value) -> Value {
    return arith::ConstantIntOp::create(b, loc, value, 64);
  };
  const auto eq = [&](Value a, Value d) -> Value {
    return arith::CmpIOp::create(b, loc, arith::CmpIPredicate::eq, a, d);
  };
  auto descriptor =
      scf::IfOp::create(b, loc, TypeRange{b.getI64Type(), b.getI64Type(), b.getI64Type()},
                        arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne, root, c(0)), true);
  {
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&descriptor.getThenRegion().front());
    auto layout = heap.read(root, c(1));
    auto invalid = arith::XOrIOp::create(b, loc, eq(layout, c(W::layout(element, boolean))),
                                         arith::ConstantIntOp::create(b, loc, 1, 1));
    auto checked = scf::IfOp::create(b, loc, invalid, false);
    {
      OpBuilder::InsertionGuard failure(b);
      b.setInsertionPointToStart(&checked.getThenRegion().front());
      heap.fault(99);
    }
    auto length = heap.read(root, c(2));
    auto external =
        arith::CmpIOp::create(b, loc, arith::CmpIPredicate::ne,
                              arith::AndIOp::create(b, loc, root, c(W::ExternalHandle)), c(0));
    auto origin = arith::SelectOp::create(b, loc, external, heap.read(root, c(3)), c(0));
    auto offset =
        arith::SelectOp::create(b, loc, external, c(0), arith::AddIOp::create(b, loc, root, c(4)));
    scf::YieldOp::create(b, loc, ValueRange{length, origin, offset});
    b.setInsertionPointToStart(&descriptor.getElseRegion().front());
    scf::YieldOp::create(b, loc, ValueRange{c(0), c(0), c(0)});
  }
  heap.store(A::Root, root);
  heap.store(A::Length, descriptor.getResult(0));
  heap.store(A::Origin, descriptor.getResult(1));
  heap.store(A::Data, descriptor.getResult(2));
  heap.store(A::Layout, c(W::layout(element, boolean)));
}

Value emitDeviceAggregateReturn(DeviceHeapEmitter &heap, OpBuilder &b,
                                const TypedModule &module, const DeviceAggregateReturnFrame &frame,
                                unsigned source) {
  using F = DeviceAggregateReturnFrame;
  using W = runtime::DeviceArrayWireABI;
  using A = runtime::DeviceAggregateEnvelopeABI;
  const auto loc = b.getUnknownLoc();
  const auto c = [&](std::uint64_t n) -> Value { return arith::ConstantIntOp::create(b, loc, n, 64); };
  const auto compare = [&](arith::CmpIPredicate pred, Value lhs, Value rhs) -> Value {
    return arith::CmpIOp::create(b, loc, pred, lhs, rhs);
  };
  const auto eq = [&](Value lhs, Value rhs) { return compare(arith::CmpIPredicate::eq, lhs, rhs); };
  const auto ne = [&](Value lhs, Value rhs) { return compare(arith::CmpIPredicate::ne, lhs, rhs); };
  const auto add = [&](Value lhs, Value rhs) -> Value { return arith::AddIOp::create(b, loc, lhs, rhs); };
  const auto both = [&](Value lhs, Value rhs) -> Value { return arith::AndIOp::create(b, loc, lhs, rhs); };
  const auto state = [&](F::Cell cell) { return heap.load(frame.start + cell); };
  const auto set = [&](F::Cell cell, Value value) { heap.store(frame.start + cell, value); };
  const auto branch = [&](Value condition, auto yes, auto no) {
    auto op = scf::IfOp::create(b, loc, condition, true);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&op.getThenRegion().front());
    yes();
    b.setInsertionPointToStart(&op.getElseRegion().front());
    no();
  };
  const auto when = [&](Value condition, auto body) {
    auto op = scf::IfOp::create(b, loc, condition, false);
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&op.getThenRegion().front());
    body();
  };
  const auto handleCell = [&](Value leaf) { return add(c(frame.handles()), leaf); };
  const auto fieldCell = [&](Value leaf, unsigned field) {
    return add(c(frame.metadata() + A::HeaderWords + field),
               arith::MulIOp::create(b, loc, leaf, c(A::LeafWords)));
  };

  enum Phase : unsigned { Start, Enumerate, FindAlias, Allocate, WriteMetadata, WriteRegions, Publish };
  const auto phase = state(F::Phase);
  const auto nodeIndex = state(F::Node);
  const auto leafCount = c(frame.projection.leaves.size());
  when(eq(phase, c(Start)), [&] {
    heap.store(frame.metadata(), c(A::Magic));
    heap.store(frame.metadata() + 1, leafCount);
    set(F::Words, c(frame.metadataWords()));
    set(F::Phase, c(Enumerate));
  });
  when(eq(phase, c(Enumerate)), [&] {
    const auto &nodes = frame.projection.nodes;
    unsigned leaf = 0;
    for (unsigned i = 0; i < nodes.size(); ++i) {
      const auto &node = nodes[i];
      when(eq(nodeIndex, c(i)), [&] {
        Value active = c(1);
        if (node.parent != DeviceAggregateOccurrence::none) {
          active = heap.load(frame.activity() + node.parent);
          if (node.activeTag) {
            const auto tag = heap.load(source + nodes[node.parent].deviceWord);
            active = arith::SelectOp::create(b, loc, eq(tag, c(node.activeTag)), active, c(0));
          }
        }
        heap.store(frame.activity() + i, active);
        set(F::Node, c(i + 1));
        if (!node.children.empty() || deviceInlineRecordValue(node.type))
          return;
        const auto value = heap.load(source + node.deviceWord);
        const auto field = frame.metadata() + A::HeaderWords + A::LeafWords * leaf;
        if (!node.type.array) {
          if (node.discriminator) {
            const auto variants = module.records.at(*nodes[node.parent].type.record).variants.size();
            auto invalid = arith::OrIOp::create(b, loc, eq(value, c(0)),
                compare(arith::CmpIPredicate::ugt, value, c(variants)));
            when(both(ne(active, c(0)), invalid), [&] { heap.fault(99); });
          }
          heap.store(field + A::Scalar, arith::SelectOp::create(b, loc, ne(active, c(0)), value, c(0)));
          return;
        }
        when(both(ne(active, c(0)), ne(value, c(0))), [&] {
          const bool boolean = node.type.builtin == BuiltinValue::Bool;
          const auto layout = W::layout(boolean ? NumericType::UInt8 : node.type.element, boolean);
          when(ne(heap.read(value, c(1)), c(layout)), [&] { heap.fault(99); });
          const auto length = heap.read(value, c(2));
          heap.store(field + A::Length, length);
          auto external = ne(arith::AndIOp::create(b, loc, value, c(W::ExternalHandle)), c(0));
          branch(external, [&] { heap.store(field + A::Origin, heap.read(value, c(3))); }, [&] {
            when(ne(length, c(0)), [&] {
              heap.store(frame.handles() + leaf, value);
              set(F::Leaf, c(leaf));
              set(F::Scan, c(0));
              set(F::Phase, c(FindAlias));
            });
          });
        });
      });
      if (!deviceInlineRecordValue(node.type))
        ++leaf;
    }
    when(eq(nodeIndex, c(nodes.size())), [&] {
      heap.begin(state(F::Words), W::layout(NumericType::UInt64, false));
      set(F::Phase, c(Allocate));
    });
  });
  when(eq(phase, c(FindAlias)), [&] {
    auto leaf = state(F::Leaf);
    auto scan = state(F::Scan);
    auto offset = fieldCell(leaf, A::Offset);
    branch(eq(scan, leaf), [&] {
      auto total = state(F::Words);
      auto length = heap.loadAt(fieldCell(leaf, A::Length));
      auto remaining = arith::SubIOp::create(b, loc, c(W::MaxWords - 4), total);
      branch(compare(arith::CmpIPredicate::ule, length, remaining), [&] {
        heap.storeAt(offset, total);
        set(F::Words, add(total, length));
        set(F::Phase, c(Enumerate));
      }, [&] { heap.fault(3); });
    }, [&] {
      branch(eq(heap.loadAt(handleCell(scan)), heap.loadAt(handleCell(leaf))), [&] {
        heap.storeAt(offset, heap.loadAt(fieldCell(scan, A::Offset)));
        heap.storeAt(handleCell(leaf), c(0)); // The first occurrence owns the export copy.
        set(F::Phase, c(Enumerate));
      }, [&] { set(F::Scan, add(scan, c(1))); });
    });
  });
  when(both(eq(phase, c(Allocate)), heap.ready()), [&] {
    set(F::Cursor, c(0));
    set(F::Phase, c(WriteMetadata));
  });
  when(eq(phase, c(WriteMetadata)), [&] {
    auto cursor = state(F::Cursor);
    heap.write(heap.handle(), add(c(4), cursor), heap.loadAt(add(c(frame.metadata()), cursor)));
    auto next = add(cursor, c(1));
    set(F::Cursor, next);
    when(eq(next, c(frame.metadataWords())), [&] {
      set(F::Leaf, c(0));
      set(F::Cursor, c(0));
      set(F::Phase, c(WriteRegions));
    });
  });
  when(eq(phase, c(WriteRegions)), [&] {
    auto leaf = state(F::Leaf);
    branch(eq(leaf, leafCount), [&] { set(F::Phase, c(Publish)); }, [&] {
      auto root = heap.loadAt(handleCell(leaf));
      auto cursor = state(F::Cursor);
      auto length = heap.loadAt(fieldCell(leaf, A::Length));
      branch(both(ne(root, c(0)), compare(arith::CmpIPredicate::ult, cursor, length)), [&] {
        auto offset = heap.loadAt(fieldCell(leaf, A::Offset));
        heap.write(heap.handle(), add(c(4), add(offset, cursor)), heap.read(root, add(c(4), cursor)));
        set(F::Cursor, add(cursor, c(1)));
      }, [&] {
        set(F::Leaf, add(leaf, c(1)));
        set(F::Cursor, c(0));
      });
    });
  });
  auto done = eq(phase, c(Publish));
  when(done, [&] {
    emitDeviceArrayReturn(heap, b, heap.handle(), NumericType::UInt64, false);
    heap.finish();
  });
  return done;
}
} // namespace kalandra
