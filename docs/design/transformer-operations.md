# Transformer tensor operations: implementation contract

This document records the implemented contract of the native AI performance
plan. CPU and Metal operations passed optimized, debug, ASan/UBSan and TSan
qualification on the development Mac. This does not qualify other physical GPUs
or a complete model-generation engine.

The first complete block is a bias-free, single-head, unmasked, pre-normalized
encoder block. It computes row RMS normalization, distinct Q/K/V projections,
scaled dot-product attention with row softmax, an output projection/residual,
then another row RMS normalization and a 4× expanding ReLU feed-forward/residual.
Epsilon and attention scale are explicit inputs. Gain vectors are identity.
There is no tokenizer, autoregressive KV cache, masking or full-model generation.

## Language operations

- `transpose(values)` accepts a rank-two numeric tensor and returns a new
  immutable tensor with axes reversed. It preserves every element's bits,
  including NaN payloads, signed zero and integer extremes. General strides and
  empty axes are supported on CPU; device backing is never mutated in place.
- `softmaxRows(values)` accepts a rank-two float32/float64 CPU tensor. Each
  nonempty row subtracts its maximum, computes exponentials, reduces them with
  the existing adjacent-pair tree, then divides each exponential by that sum.
  Empty outputs stay empty. A NaN-containing row and an all-negative-infinity row
  produce NaNs. Positive infinities split probability equally among positive
  infinities, with positive zero elsewhere.
- `rmsNormRows(values, epsilon)` accepts a rank-two floating tensor and a scalar
  of the same type. Epsilon must be finite and strictly positive; invalid epsilon
  faults as Arithmetic. A row separately rounds squares, reduces them with the
  adjacent-pair tree, divides by the column count, adds epsilon, then divides each
  original element by the square root. It does not apply learned gain or bias.

All operations preserve source input immutability and existing shape/allocation,
first-fault, ownership and cancellation rules. They are distinct typed operations,
not pointwise maps, so map fusion cannot accidentally discard row dependencies.

## Numerical policy

Transposition works under hardware/software arithmetic policies because it only
moves bits. The initial row math implementation requires explicit/default
hardware arithmetic. Software-policy compilation of row math is rejected clearly
instead of silently executing hardware math. CPU supports float32 and float64 row
math; the first Metal row-math implementation supports float32. Other numeric
types are rejected for row math, rather than implicitly converted or emulated.

Existing addition, multiplication and reduction order are unchanged. Exponential
evaluation has backend-dependent accuracy; it does not promise
identical exp bits across CPU/GPU math implementations. Metal uses precise math
with fast math and contraction disabled. Apple's Metal language specification
(4 June 2026, section 8.4, pages 368–370) bounds single-precision exp error at four
ULPs and specifies correctly rounded square root in this mode. Device denormal
handling remains governed by its documented hardware arithmetic policy. Source:
[Apple Metal Shading Language Specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).

CPU exp uses the platform math library. The current Mac corpus is checked with
a four-ULP exp interval propagated through the rounded sum and division; that
test budget is not a universal accuracy guarantee for every platform's libm.
Each new platform must pass its own numerical qualification.

Qualification compares each operation, intermediate block stage and final
output against an independent reference with propagated rounding/math-error
bounds. Matrix kernels retain separately rounded products and the exact adjacent
tree; vendor reference FMA/reassociation is disclosed separately. Neither a
checksum alone nor a broad empirical tolerance establishes numerical correctness.

## Execution architecture

CPU lowering emits native loops with one reusable row workspace, normal checked
allocation and bounded cooperative checkpoints. Exp/sqrt lower to standard CPU
math operations; no vendor matrix library is linked.

Metal transforms are trusted compiler-produced kernels in the existing numerical
program path. Row operations use one workgroup per row and owned global scratch,
so row length is not silently capped by fixed threadgroup storage. Every reduction
level uses disjoint aligned subtrees and device-memory barriers. Workspaces and
parameters remain charged until device acknowledgment. Transposition copies raw
bytes and changes shape metadata. No operation is implemented by a CPU reference
while being reported as GPU execution.

The optimized GPU comparison is a separate MPS/MPSGraph implementation of the
same complete block. It is a benchmark-only dependency. Timing must disclose
precision, fusion, preparation, allocation, uploads, residency and completion.
