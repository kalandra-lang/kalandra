# Checked CPU tensor numeric conversions

Numeric type-name calls convert CPU tensors among all ten implemented element types: `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64`. All 100 source/destination pairs are supported, including identity conversions. The aliases `int`, `uint` and `float` still select their 64-bit types.

```kalandra
fn compact(values: Tensor[int64, ?]) -> Tensor[int8, ?]:
  int8(values)

fn main() -> Tensor[int8, ?] ! TaskError:
  values: Tensor[int64, 4] = tensor([-128, -1, 0, 127])
  try parallel:
    try await spawn compact(values)
```

This prints `tensor([-128, -1, 0, 127], [4])`. The source keeps its original type and contents; the result uses one-byte cells. Rank and every extent remain unchanged, including dynamic extents, rank-zero tensors and empty axes. The destination type does not give the input a new literal context.

## Element rules

Each element uses the existing checked scalar conversion engine:

- Integer-to-integer conversions require the value to fit the destination range. They do not wrap, saturate or reinterpret the sign bit.
- Floating-point values are truncated toward zero before integer range checking. Thus `uint8` conversion accepts −0.9 as zero, but rejects −1, 256, NaN and infinity.
- Integer-to-float and float-width conversions use round-to-nearest, ties-to-even. A finite value that overflows the floating destination faults; representable subnormals and rounded zero remain valid.
- Identity conversions preserve every bit. Changed-width floating conversions canonicalize NaNs according to the existing numeric contract.

A failing lane raises an Arithmetic fault at the conversion expression. No partial result is published, even if earlier lanes succeeded. Tensor literal conversion checks currently run at execution time; this does not add tensor constant folding. Source and destination byte counts are checked separately. Allocation failure and cooperative cancellation settle intermediate storage without returning a successful tensor.

## Workers and limits

Ordinary helper calls and direct calls such as `spawn int8(values)` use the same conversion. Retained inputs and outputs survive nested workers, repeated awaits, recovery and source invocation settlement. Numeric arrays still require explicit per-element mapping; a tensor conversion does not implicitly accept an array. Mixed-type tensor arithmetic still requires explicit conversion.

Conversions operate on CPU tensors. GPU-resident values must first be transferred to CPU; GPU numeric casts remain unavailable. This step changes admission and validation coverage, while reusing the conversion engine, CPU tensor lowering, allocator and scheduler.

## Qualification

The [shared tensor harness](../../native/tests/TensorConversionRuntimeTests.cpp) uses the independent [APInt/APFloat reference](../../native/tests/ConversionOracle.h) across all 100 pairs. Rank-two checks use the full valid scalar corpus, including every 8/16-bit source representation; invalid values are sampled across the corpus and placed in first, middle and last lanes. Representative pairs also cover ranks zero, one and three. Tests check exact output bits in eight floating-point environments, strided/transposed views, source immutability, retained results, empty shapes, metadata and allocation faults, and cancellation.

The [compiled driver](../../native/cmake/VerifyTensorConversions.cmake) partitions checks by source type. Worker checks cover every source/destination pair, identity dispatch, nested and cached awaits, fault recovery, empty results, cancellation/deadline/admission handling and allocation-failure sweeps. Debug, optimized, ASan/UBSan and TSan variants use the same contracts.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-narrow-tensor-conversions-2026-10-19.json) records the source hashes, exact-bit checks and final regression scope.
