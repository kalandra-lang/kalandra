#pragma once
#include "mlir/Support/LogicalResult.h"
namespace mlir {
class ModuleOp;
}
namespace kalandra {
// The pinned GPU-to-SPIR-V conversion does not lower GPUFuncOp workgroup
// attributions. Materialize only their scalar loads/stores as SPIR-V workgroup
// pointers first; numerical operations and barriers keep their normal lowering.
mlir::LogicalResult lowerWorkgroupSpirv(mlir::ModuleOp);
} // namespace kalandra
