#pragma once
#include "../gpu-runtime/ControlArena.h"
#include "mlir/IR/Builders.h"
#include <span>
#include <vector>
namespace kalandra {
// Compiler emitter for a checkpointed device allocator. All traversal and
// reachability operations below become device IR; the host only grows backing.
struct DeviceRootMap {
  std::size_t pc;
  std::vector<std::size_t> cells;
};
class DeviceHeapEmitter {
public:
  // The first four cells are the runtime PC/capacity/request/resume ABI. The
  // allocator service state is private to the compiler's invocation frame.
  enum Cell : unsigned {
    PC,
    Capacity,
    Requested,
    Resume,
    Phase,
    Cursor,
    Need,
    Layout,
    Length,
    RootCursor,
    PreviousFree,
    Collected,
    GrowStart,
    Allocation,
    Cells
  };
  static_assert(PC == runtime::detail::ControlArenaABI::PC &&
                Capacity == runtime::detail::ControlArenaABI::Capacity &&
                Requested == runtime::detail::ControlArenaABI::Requested &&
                Resume == runtime::detail::ControlArenaABI::Resume);
  DeviceHeapEmitter(mlir::OpBuilder &, mlir::Value arena, mlir::Value params,
                    unsigned parameterWords, std::span<const DeviceRootMap> roots);
  mlir::Value load(unsigned cell);
  void store(unsigned cell, mlir::Value);
  void initialize();
  // Execute one bounded service step. True means the source instruction may run
  // (normal execution, or initialization of a newly allocated result).
  mlir::Value service();
  void begin(mlir::Value length, unsigned layout);
  mlir::Value ready();
  mlir::Value handle();
  mlir::Value cursor();
  void advance();
  void finish();
  mlir::Value read(mlir::Value handle, mlir::Value field);
  void write(mlir::Value handle, mlir::Value field, mlir::Value);
  void fault(unsigned code);

private:
  mlir::OpBuilder &b;
  mlir::Location loc;
  mlir::Value arena, params;
  unsigned parameterWords;
  std::span<const DeviceRootMap> roots;
  mlir::Value c(std::uint64_t);
  mlir::Value add(mlir::Value, mlir::Value);
  mlir::Value eq(mlir::Value, mlir::Value);
  mlir::Value loadAt(mlir::Value);
};
} // namespace kalandra
