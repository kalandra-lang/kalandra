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
  ScalarControl
};
} // namespace kalandra
