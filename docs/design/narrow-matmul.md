# Checked narrow CPU matrix multiplication

`matmul` accepts rank-two CPU tensors with matching `int8`, `uint8`, `int16` or `uint16` elements, in addition to the existing 32/64-bit integer and floating-point types. The output keeps the input element type. The left column count must match the right row count; output dimensions are the left row count and right column count.

```kalandra
fn multiply(a: Tensor[int8, ?, ?], b: Tensor[int8, ?, ?]) -> Tensor[int8, ?, ?]:
  matmul(a, b)

fn main() -> Tensor[int8, ?, ?] ! TaskError:
  a: Tensor[int8, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
  b: Tensor[int8, 3, 2] = tensor([[1, 2], [1, 2], [1, 2]])
  try parallel:
    try await spawn multiply(a, b)
```

This prints `tensor([6, 12, 15, 30], [2, 2])`. Workers retain the exact-width inputs and result through nested calls and repeated awaits. Source matrices remain unchanged.

## Arithmetic and failure contract

Every product must fit the element type before entering the balanced adjacent-pair addition tree. Every addition must also fit. There is no implicit wider accumulator, wrapping, saturation or result promotion. The compiler uses wider internal arithmetic to detect overflow; those intermediates do not change the source-level result type.

For example, multiplying an `int8` row `[64, -64]` by a column `[2, 2]` faults on the first product, even though the mathematical dot product is zero. A product that fits can still overflow in a later addition. The existing reduction order remains part of the numerical contract.

A zero-length contraction produces a correctly shaped zero matrix. Zero rows or columns produce a correctly shaped empty result. Dynamic extents, element counts and allocation sizes are checked before unsafe access. Offset and strided inputs use logical matrix coordinates.

Arithmetic faults retain the matmul expression's source location. Worker calls report them through `TaskError.Fault`; recovery and cooperative cancellation use the existing task contract. A fault in a later cell does not publish an incomplete matrix. Scratch and partial output allocations settle on success, failure and cancellation; observed results can retain their backing after the producing invocation and scheduler are destroyed.

## Scope and qualification

CPU matrix multiplication now covers all ten implemented numeric types. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten types. Metal keeps its separately qualified float32 subset. This step reuses the CPU matrix/reduction lowering, scheduler and ownership implementation.

The [shared integer runtime harness](../../native/tests/Int32MatmulRuntimeTests.cpp) compares generated kernels against an independent wider reference. It covers signed/unsigned boundaries, random matrices, product and tree overflow, odd contraction sizes, strided/transposed input views, aliases, empty shapes, invalid metadata, allocation failures, cancellation and retained output storage. The [compiled driver](../../native/cmake/VerifyInt32Matmul.cmake) adds fixed shapes, exact endpoints, nested/cached worker results, fault recovery and cleanup with one, two and four workers. Rank, shape and mixed-type diagnostics must preserve existing output files. Debug, optimized, ASan/UBSan and TSan variants cover all four narrow types and shared wider-type regressions.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-narrow-matmul-2026-10-19.json) records the validated source hashes and regression scope.
