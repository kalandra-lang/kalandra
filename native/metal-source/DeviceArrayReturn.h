#pragma once
#include "DeviceHeap.h"
#include "../gpu-source/AggregateProjection.h"
#include "../runtime/DeviceArrayABI.h"
namespace kalandra {
// Publish only checked, device-computed descriptor cells. Runtime transfer keeps
// this invocation's arena alive; this is not a host pointer or native result.
void emitDeviceArrayReturn(DeviceHeapEmitter &, mlir::OpBuilder &, mlir::Value root,
                           NumericType element, bool boolean);

struct DeviceAggregateReturnFrame {
  enum Cell : unsigned { Phase, Node, Scan, Leaf, Words, Cursor, StateCells };
  unsigned start;
  const DeviceAggregateProjection &projection;
  unsigned activity() const { return start + StateCells; }
  unsigned handles() const { return activity() + projection.nodes.size(); }
  unsigned metadata() const { return handles() + projection.leaves.size(); }
  unsigned metadataWords() const { return runtime::DeviceAggregateEnvelopeABI::HeaderWords +
      runtime::DeviceAggregateEnvelopeABI::LeafWords * projection.leaves.size(); }
  unsigned end() const { return metadata() + metadataWords(); }
};

// One enumeration, alias search, allocation or copy step. True means the
// UInt64 envelope has been published to the existing device export ABI.
mlir::Value emitDeviceAggregateReturn(DeviceHeapEmitter &, mlir::OpBuilder &,
                                     const TypedModule &, const DeviceAggregateReturnFrame &,
                                     unsigned source);
} // namespace kalandra
