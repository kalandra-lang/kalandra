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
} // namespace kalandra
