#pragma once
namespace kalandra {
// Compiler-owned, numerical control instructions (graph edges remain forward-only). Shared by GPU
// artifact emission and backend runtimes; not a serialized public ABI.
enum class NumericProgramKind {
  Map,
  Reduce,
  Matmul,
  Branch,
  Jump,
  Alias,
  EqualShape,
  Extent,
  MatrixShape,
  ShapeFault,
  ScalarControl,
  // Explicit private-arena ABI; unknown runtimes must reject this instruction.
  DeviceControl,
  // All padding lanes participate. Uses a WorkgroupLaunch descriptor, not the
  // pointwise map's linear-domain or checkpointed single-lane control ABI.
  Workgroup
};
constexpr bool isGpuControlKind(NumericProgramKind kind) {
  return kind == NumericProgramKind::ScalarControl || kind == NumericProgramKind::DeviceControl;
}
} // namespace kalandra
