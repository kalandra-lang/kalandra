# GPU arithmetic on matrix inputs

A float32 GPU helper can transform either or both matrix operands, multiply
those results, and apply arithmetic to the product:

```kalandra
fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?],
           bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  product = matmul(input + scale, weights - 1)
  (product + bias) * scale
```

Transfer the tensor arguments explicitly, then spawn the helper on the GPU.
The input expressions support strict `+`, `-`, `*`, `/` and unary signs,
scalar lifting, bindings and transitive named/imported helpers. Tensor operands
within one elementwise expression must have identical complete shapes. The two
matrix operands need only satisfy the existing multiplication shape contract.
There is no new implicit broadcasting.

One-sided transformations, such as `matmul(input + 1, weights)`, do not create a
map for the untouched operand. Two different expressions can read the same
original tensor, as in `matmul(values + 1, values * 2)`. Reordered and repeated
helper parameters preserve operand identity. Post arithmetic is optional.

## Resident stages, one scheduled task

The bounded compiler proof still requires exactly one matrix primitive. Each
transformed operand gets an independent `kernelN-left` or `kernelN-right`
elementwise shader. Scalar SSA liveness selects that operand expression and its
contributing original inputs. Each stage has its own projected read table and
uses direct bindings or Tier 2 argument buffers as needed. The manifest records
each stage, logical signature, physical reads and binding layout.

The existing multiplication shader and tile alternatives remain unchanged.
Every input arithmetic operation rounds before multiplication; products and
the contraction tree retain their separately rounded, balanced-adjacent rules.
There is no fusion across a primitive boundary or implicit CPU fallback.

The runtime captures immutable argument packs and returns one tensor task. It
executes the left map, right map, multiplication and optional post stage in that
order, omitting absent stages. An acknowledged map result replaces only its
corresponding operand. The original captures remain available to the other map
and post stage. Intermediate tensors stay on the GPU, and only an explicit
transfer downloads the final result. This is composition, not an optimization
claim: mapped operands currently require separate output storage and commands.

Generated supervisor guards retain every transitive shape obligation before
dispatch, including obligations in otherwise dead arithmetic. Runtime checks
also validate actual mapped buffers and multiplication shapes. Empty maps hand
off their shape-preserving results without submitting a command; at most two
operand maps, one multiplication and one post stage are involved. A nonempty
output with zero contraction length still produces the existing zero product
before applying post arithmetic.

Each capture and allocation uses existing budgets and failure checkpoints.
Commands acknowledge before the next stage is submitted, so one in-flight slot
suffices. Cancellation, deadlines and device/allocation failures prevent partial
results from being published. Active resources remain owned until completion
acknowledgement; final immutable results can outlive their input/task owners.
CPU helper compilation retains its existing conservative stack admission; very
large source bodies may need ordinary helper boundaries.

## Boundaries and qualification

[One matrix product followed by one reduction](native-gpu-matmul-reduction.md)
now reuses these input stages and returns a scalar task. Further primitives,
data-dependent GPU branches,
recursive helpers, broader numeric types and higher-rank operations remain
separate lowering work. This change does not introduce custom shaders or a
public kernel ABI. Supported unused arithmetic may be removed, but additional
numerical primitives are still rejected transactionally.

[`pre-matmul.k`](../../native/metal-source/pre-matmul.k) exercises the complete
map/map/matrix/post sequence. The shared
[`MatmulPostTests.cpp`](../../native/metal-source/MatmulPostTests.cpp) harness
compares it against checked CPU execution, separately scheduled GPU stages and
LLVM APFloat across 96 cases and 4,392 result cells per build mode. Inputs include
empty/zero-inner dimensions, odd contraction lengths, subnormals, signed zeros,
infinities, NaNs and arbitrary bit encodings. It sweeps allocation checkpoints,
checks failure/cancellation/deadlines in all four commands, scheduler destruction,
a second-stage memory-budget failure and retained result ownership.

[`VerifyPreMatmul.cmake`](../../native/metal-source/VerifyPreMatmul.cmake) adds
imports/re-exports, either/both operand maps, no-post results, repeated inputs,
31 contributing map inputs using Tier 2 bindings, full-shape failures, relocation,
separate-call equivalence and byte-identical underlying multiplication shaders.
Programs run with 1/2/4 workers in Debug, optimized, ASan/UBSan and TSan modes.
The installed Release compiler also builds and relocates a complete application
with all three supplemental shaders, then runs it after compiler removal.

The [qualification record](../../native/metal-proof/evidence/gpu-pre-matmul-2026-10-21.json)
records the selected checks and their limits. General numerical graphs remain
in N2; this step adds no performance measurements.
