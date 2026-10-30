# Checked narrow CPU tensors

CPU tensors now support `int8`, `uint8`, `int16` and `uint16` construction and checked elementwise arithmetic. Element types remain distinct; literals can obtain a type from context, but existing values are not implicitly narrowed or promoted.

```kalandra
fn twice(values: Tensor[int16, ?, ?]) -> Tensor[int16, ?, ?]:
  values * 2

fn main() -> Tensor[int16, ?, ?] ! TaskError:
  values: Tensor[int16, 2, 2] = tensor([[1, 2], [3, 4]])
  try parallel:
    try await spawn twice(values)
```

The result is `tensor([2, 4, 6, 8], [2, 2])`. Source tensors and worker storage keep the exact one- or two-byte element width.

## Construction and operations

Rank-zero tensors, nested literals, explicit flat-data shapes and typed empty tensors use the existing tensor model. Addition, subtraction, multiplication, division, remainder, unary plus and unary minus reuse the shared checked integer lowering. Same-typed tensors require compatible shapes. Scalar lifting works on either side and preserves the tensor's element type.

Each arithmetic result must fit its element type. Compile-time literal/range errors remain diagnostics; dynamic overflow and zero divisors produce Arithmetic faults. Signed division truncates toward zero; minimum divided by −1 overflows, while its remainder is zero. Unsigned negation accepts only zero. A failure does not expose a partially computed result.

Existing indexing, shape queries, reshaping, array-to-tensor construction and tensor-to-array flattening preserve element identity and logical ordering. These are structural operations, not numeric conversions. Their existing bounds, size and ownership checks still apply. Reshaping and conversions between array/tensor containers retain the current copying implementation.

## Records, workers and display

Narrow tensor fields can appear in finite CPU records alongside scalar, array and other supported fields. Immutable updates and local worker calls retain their storage through nested tasks, repeated awaits, source settlement and error recovery. Source values remain unchanged. Observed results can retain their data after their producing invocation and scheduler are destroyed.

CPU command-line programs print narrow tensor elements as signed or unsigned decimal numbers and include the shape. Existing display limits apply, including for empty and rank-zero tensors.

## Separate operation gates

[Checked narrow CPU `reduceSum`](narrow-reductions.md) is supported. [Checked narrow CPU `matmul`](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all 100 pairs among the ten implemented numeric types, including narrow identity casts. Both source and destination retain their own numeric identity and shape checks. Arrays still require explicit scalar mapping for numeric conversion.

Narrow GPU execution is also unavailable. Apple Metal qualification remains separate from CPU elementwise support; NVIDIA/CUDA has its [own roadmap](../native-cuda-roadmap.md). Device transfers and source capability checks must not treat an available CPU storage type as a supported GPU arithmetic type.

## Qualification

The shared integer tensor harness is parameterized by element width and signedness. It checks boundary/random values, scalar lifting, ranks 0–3, empty shapes, strided borrowed input, aliasing, shape refinement, arithmetic/size faults, allocation failure, active cancellation and retained results. Shared tensor-worker checks exercise exact payload bits, cached observations, wrong-layout rejection, one/four workers, cancellation, deadlines, admission, allocator failures and lifetime settlement.

Executable examples cover record fields, array/tensor adapters, reshaping, indexing, signed/unsigned endpoint display, empty tensors, mixed element types and error recovery. Rejected operations must preserve existing output artifacts. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-narrow-tensors-2026-10-18.json) records the final checks and scope.
