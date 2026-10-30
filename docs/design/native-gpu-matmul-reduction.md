# GPU matrix multiplication followed by reduction

A float32 GPU helper can now return the reduction of one matrix product:

```kalandra
fn score(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> float32:
  reduceSum(matmul(input, weights))
```

Bindings and named/imported helpers can forward the product to the reduction.
The existing matrix input arithmetic also composes with this path:

```kalandra
fn score(input: Tensor[float32, ?, ?], scale: float32,
         weights: Tensor[float32, ?, ?]) -> float32:
  product = matmul(input + scale, weights - 1)
  reduceSum(product)
```

Transfer inputs explicitly, then spawn the helper on the GPU. It returns one
scalar task. Only the final four-byte scalar is read back; the product and any
transformed operands stay on the GPU. Separate explicit placed calls remain
available when a program needs the product as an independently observable value.

## Bounded composition and numerical rules

The compiler proves one supported `matmul` consumed by one supported `reduceSum`
through the checked helper graph. It tracks matrix operand identities and input
maps, validates every statement, and rejects extra primitives rather than
silently dropping them. This is the first dependent pair of numerical
primitives within one source entry, not arbitrary graph support.

The source entry emits the unchanged multiplication shader and qualified tile
alternatives, plus a `kernelN-reduce` shader using the unchanged reduction tree.
Optional `kernelN-left`/`kernelN-right` input shaders retain their independent
projected direct or Tier 2 argument layouts. The manifest records each stage.

Input arithmetic rounds before multiplication. Products are separately rounded;
each matrix contraction uses the established balanced-adjacent tree. The final
reduction then applies its own balanced-adjacent tree to the row-major matrix
cells. It does not flatten all multiplication terms into one different tree or
fuse additions across those boundaries. CPU execution, separate GPU calls and
the composed helper preserve this contract, with NaNs compared by classification
in numerical qualification.

All transitive shape obligations remain supervisor preflight guards. This
includes input-map shape equality, declared parameter extents, and matrix inner
dimensions. Runtime validation checks buffers, kernels, device residence and
stage interfaces as well. No unsupported operation falls back to the CPU.

## One scalar task owns the stages

The runtime's existing acknowledged-stage handoff now connects a matrix command
to reduction. Optional left/right maps precede multiplication; each acknowledged
result replaces only its corresponding operand. The matrix becomes the
reduction input after its command acknowledges. Original argument packs remain
immutable, and task completion publishes only the final scalar layout.

Zero-cell products return the canonical positive-zero empty sum without a
reduction command. Shapes with a zero contraction length but nonempty output
still produce zero matrix cells and reduce those cells with the normal tree.
Empty input-map stages retain their shapes and continue to the matrix stage.

One in-flight slot suffices. Existing memory/capture budgets and allocation
failure checkpoints apply. Cancellation, deadlines and device failures stop
later submissions; active command resources stay owned until acknowledgement.
Failure in reduction preparation never publishes the matrix as a scalar result.
Cached scalar observations remain valid after original input owners are released.

## Current boundaries

[Arithmetic between the product and reduction](native-gpu-matmul-adjust-reduction.md)
now has its own qualified resident stage, including tensor bias and scalar
operations. [Scalar arithmetic after the pair](native-gpu-matmul-scalar-reduction.md)
now also stays on GPU. More matrix or reduction primitives, recursive helpers
and data-dependent GPU branches remain
separate lowering work. Programs can express those calculations with separate
placed tasks today. Broader GPU numeric types/ranks and custom shader interfaces
remain separate roadmap items. Existing conservative CPU stack admission is
unchanged; large helper bodies can still require ordinary helper boundaries.

## Qualification

[`MatmulReductionTests.cpp`](../../native/metal-source/MatmulReductionTests.cpp)
checks 192 plain/transformed matrix cases per mode against checked CPU execution,
separate GPU calls and an independent LLVM APFloat reference. Cases cover empty
outputs, zero contraction lengths, odd tree lengths, signed zeros, subnormals,
infinities, NaNs and arbitrary bit encodings. It sweeps 43 allocation checkpoints
and checks cancellation/deadline/device failure in all four possible command
stages, pending scheduler destruction, invalid reduction kernels, a reduction
scratch budget failure, one in-flight slot and cached scalar ownership.

[`VerifyMatmulReduction.cmake`](../../native/metal-source/VerifyMatmulReduction.cmake)
qualifies imports/re-exports, plain/either/both operand maps, repeated operands,
31 contributing map inputs through Tier 2 bindings, shape failures, application
relocation and byte-identical multiplication/reduction artifacts. Programs run
with 1/2/4 workers across Debug, optimized, ASan/UBSan and TSan modes. Source-only
checks preserve outputs on unsupported compositions. The installed Release
compiler builds and relocates an application carrying its input/reduction
shaders; the application runs after the compiler installation is removed.

The [qualification record](../../native/metal-proof/evidence/gpu-matmul-reduction-2026-10-21.json)
records the selected checks. This change makes no performance improvement claim.
