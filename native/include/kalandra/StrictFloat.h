#pragma once
#include "kalandra/Arithmetic.h"
#include "mlir/Support/LogicalResult.h"
namespace mlir {
class Operation;
}
namespace kalandra {
// Scalar binary32 add/subtract/multiply/divide and bit-preserving negation -> integer IEEE
// round-to-nearest/ties-to-even. No target precision relaxation. Other floating arith operations
// fail closed.
mlir::LogicalResult lowerStrictFloat32(mlir::Operation *root);
// Binary64 arithmetic using only 64-bit integer words. GPU entry boundaries must
// carry its bits as i64; canonicalization removes temporary f64 bitcasts before
// SPIR-V conversion. This does not enable source/ABI capabilities by itself.
mlir::LogicalResult lowerStrictGpuFloat64(mlir::Operation *root);
// CPU-only binary32/binary64 implementation. Binary64 uses i128 intermediates;
// this entry point rejects GPU/SPIR-V modules. Source admission and typed
// native/worker ABI validation belong to their respective compiler layers.
mlir::LogicalResult lowerStrictCpuFloat(mlir::Operation *root);
mlir::LogicalResult lowerOutlinedStrictCpuFloat(mlir::Operation *operation);
mlir::LogicalResult lowerMetalHardwareFloat(mlir::Operation *root, bool allowSoftwareFallback);
} // namespace kalandra
