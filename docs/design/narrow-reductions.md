# Checked narrow CPU reductions

`reduceSum` accepts CPU tensors with `int8`, `uint8`, `int16` and `uint16` elements, as well as the existing 32/64-bit integer and floating-point types. The result has the same element type as the input. There is no implicit wider accumulator or result promotion.

```kalandra
fn total(values: Tensor[int8, ?]) -> int8:
  reduceSum(values)

fn main() -> int8 ! TaskError:
  values: Tensor[int8, 5] = tensor([42, 1, 2, 3, 4])
  try parallel:
    try await spawn total(values)
```

This prints `52`. Workers retain the input tensor and return an exact-width scalar. Nested tasks and repeated awaits use the existing scheduler and ownership rules.

## Arithmetic order and faults

The shared CPU reduction engine reads elements in logical row-major order, including offset and strided input views. It adds adjacent pairs, carries an unpaired final element unchanged, and repeats until one value remains. Every addition is checked against the input element type's range. An empty tensor returns a same-typed zero; a rank-zero tensor returns its only element.

For `int8`, `[127, 1, -1]` faults on the first pair even though the mathematical sum is 127. `[127, -1, 1]` succeeds and returns 127. The reduction tree is part of the numerical contract; widening the accumulator or reassociating additions would change that contract.

A synchronous overflow produces an Arithmetic fault at the reduction expression. Inside a worker it is reported through `TaskError.Fault`, with the original source location. Recovery, cooperative cancellation and allocation failures follow the existing task contract. Scratch storage uses one- or two-byte cells and is released when the invocation settles; faults expose no partial result.

## Scope and qualification

This extends reduction admission, not the reduction engine, scheduler, tensor ABI or allocator. CPU reductions now cover all ten implemented scalar types. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten types. Metal still admits only its qualified float32 reduction subset.

The [shared integer runtime harness](../../native/tests/Int32ReductionRuntimeTests.cpp) checks ranks zero through three against an independent wider reference, including boundary/random inputs, odd lengths, empty axes, strides, allocation faults and cancellation. The [compiled driver](../../native/cmake/VerifyInt32Reductions.cmake) also checks nested/cached worker calls, recovery, fault locations, allocation cleanup and rejection of unsupported Metal execution. Debug, optimized, ASan/UBSan and TSan runs cover all four narrow types; the same harness continues to cover 32/64-bit integers.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-narrow-reductions-2026-10-18.json) records the validated source hashes and regression scope.
