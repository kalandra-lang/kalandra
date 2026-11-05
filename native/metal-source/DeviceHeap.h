#pragma once
#include "../gpu-runtime/ControlArena.h"
#include "kalandra/NumericType.h"
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
// Guards are ordered from the outermost enum to the innermost. A failed
// guard skips its entire reference, including any inactive nested tags.
struct DeviceReferenceGuard {
  unsigned cell;
  std::uint64_t tag;
};
struct DeviceReferenceField {
  unsigned cell;
  std::vector<DeviceReferenceGuard> guards;
};
struct DeviceTraceLayout {
  unsigned identity, stride;
  std::vector<DeviceReferenceField> references;
};
struct DeviceGraphExportLayout {
  unsigned arenaIdentity, wireIdentity, stride;
  std::vector<unsigned> references;
};
struct DeviceArrayInput {
  mlir::Value buffer, words;
  unsigned layout;
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
    SortPhase,
    SortHandle,
    SortBuild,
    SortEnd,
    SortRoot,
    SortResume,
    GreyHead,
    TraceHandle,
    TraceElement,
    TraceField,
    Cells
  };
  static_assert(PC == runtime::detail::ControlArenaABI::PC &&
                Capacity == runtime::detail::ControlArenaABI::Capacity &&
                Requested == runtime::detail::ControlArenaABI::Requested &&
                Resume == runtime::detail::ControlArenaABI::Resume);
  DeviceHeapEmitter(mlir::OpBuilder &, mlir::Value arena, mlir::Value params,
                    unsigned parameterWords, std::span<const DeviceRootMap> roots,
                    std::span<const DeviceTraceLayout> layouts = {},
                    std::span<const DeviceArrayInput> inputs = {}, bool readOnlyBlocks = false);
  mlir::Value load(unsigned cell);
  void store(unsigned cell, mlir::Value);
  // Dynamic indices into compiler-reserved frame tables.
  mlir::Value loadAt(mlir::Value cell);
  void storeAt(mlir::Value cell, mlir::Value value);
  void initialize();
  // Copy one word from a host-validated, read-only arena image at entry.
  // True permits the caller to publish the root and leave initialization.
  mlir::Value seed(mlir::Value image, mlir::Value sealedWords, unsigned offset);
  // Execute one bounded service step. True means the source instruction may run
  // (normal execution, or initialization of a newly allocated result).
  mlir::Value service(bool terminalTrace = false);
  // Trace result roots without sweeping. The terminal exporter consumes the
  // marked graph while the arena remains quiescent.
  void beginResultTrace();
  mlir::Value resultTraceReady();
  // Number one marked block per step and measure the canonical payload.
  mlir::Value numberResultBlocks();
  mlir::Value resultNodeCount();
  mlir::Value resultPayloadWords();
  mlir::Value resultNodeId(mlir::Value handle);
  // Append a terminal UInt64 transfer block without invoking collection again.
  mlir::Value reserveResultEnvelope(mlir::Value words, unsigned baseCell);
  // One block header or one payload word per checkpoint, up to the original
  // arena boundary. Return true only after the whole graph has been written.
  mlir::Value writeResultEnvelope(mlir::Value root, unsigned baseCell, unsigned blockCell,
                                 unsigned wordCell, unsigned outputCell,
                                 std::span<const DeviceGraphExportLayout>);
  void begin(mlir::Value length, unsigned layout);
  mlir::Value ready();
  mlir::Value handle();
  mlir::Value cursor();
  void advance();
  void finish();
  mlir::Value read(mlir::Value handle, mlir::Value field);
  void write(mlir::Value handle, mlir::Value field, mlir::Value);
  void writeSource(mlir::Value handle, mlir::Value field, mlir::Value);
  void requireWritable(mlir::Value handle);
  void fault(unsigned code);
  // One bounded heapsort comparison/swap per control step, preserving raw bits.
  void beginSort(mlir::Value handle, mlir::Value count);
  mlir::Value sortStep(NumericType element, bool boolean);

private:
  mlir::OpBuilder &b;
  mlir::Location loc;
  mlir::Value arena, params;
  unsigned parameterWords;
  std::span<const DeviceRootMap> roots;
  std::span<const DeviceTraceLayout> layouts;
  std::span<const DeviceArrayInput> inputs;
  bool readOnlyBlocks;
  mlir::Value markState(mlir::Value);
  mlir::Value readOnlyFlag(mlir::Value);
  mlir::Value readArena(mlir::Value, mlir::Value);
  void writeArena(mlir::Value, mlir::Value, mlir::Value);
  mlir::Value readInput(mlir::Value, mlir::Value);
  void markArena(mlir::Value);
  void mark(mlir::Value handle);
  mlir::Value c(std::uint64_t);
  mlir::Value add(mlir::Value, mlir::Value);
  mlir::Value eq(mlir::Value, mlir::Value);
};
} // namespace kalandra
