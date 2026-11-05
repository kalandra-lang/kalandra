#pragma once
#include "DeviceHeap.h"
namespace kalandra {
// Publish only checked, device-computed descriptor cells. Runtime transfer keeps
// this invocation's arena alive; this is not a host pointer or native result.
void emitDeviceArrayReturn(DeviceHeapEmitter &, mlir::OpBuilder &, mlir::Value root,
                           NumericType element, bool boolean);
} // namespace kalandra
