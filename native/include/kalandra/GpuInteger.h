#pragma once
#include "kalandra/NumericType.h"
#include "mlir/IR/Value.h"
#include <optional>

namespace mlir {
class OpBuilder;
class Location;
} // namespace mlir
namespace kalandra {
enum class GpuIntegerOperation { Add, Subtract, Multiply, Divide, Remainder, Negate };
struct GpuIntegerResult {
  mlir::Value bits, valid;
};
// Checked arithmetic on i64 bit carriers. Results are masked to the source
// width; invalid results must not be published by the device task protocol.
// Every emitted intermediate is at most 64 bits, including int64 multiplication.
std::optional<GpuIntegerResult> emitCheckedGpuInteger(mlir::OpBuilder &, mlir::Location,
                                                      NumericType, GpuIntegerOperation,
                                                      mlir::Value left, mlir::Value right);
} // namespace kalandra
