#pragma once
namespace kalandra {
// Compiler-owned, forward-only numerical control instructions. Shared by GPU
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
  ShapeFault
};
} // namespace kalandra
