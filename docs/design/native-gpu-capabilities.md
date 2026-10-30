# Native Apple GPU capability matrix

**Current execution policy:** [Hardware arithmetic and explicit software fallback](hardware-arithmetic.md) is the default policy as of 2026-10-24. CPU float32/float64 and Metal float32 use hardware arithmetic. Metal float64 arithmetic requires explicit fallback permission. Exact-bit software proofs below describe the opt-in software implementation, not the default hardware path.

This describes the current `--target metal` source backend, rather than every
operation the runtime or CPU compiler can represent. **GPU** means computation
inside a dispatched kernel; **Host** means the generated CPU coordinator in a
mixed CPU/Metal program. Host support does not imply equivalent shader support.
Native CPU numeric widths, Strings, records, enums and managed closures are not
automatically device capabilities. Transfers remain explicit; an unsupported
GPU program is rejected rather than silently run on the CPU.

## Tested boundaries

<!-- capability-matrix:start -->

| Feature | Runs in | Status / boundary | Checks |
| --- | --- | --- | --- |
| Vector elementwise expressions | GPU | **Supported.** float32; strict +, -, * and composed helpers | [source](../../native/metal-source/composed.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/source-gpu-2026-10-15.json) |
| Matrix elementwise expressions | GPU | **Supported.** Rank two; equal complete shapes | [source](../../native/metal-source/matrix-elementwise.k), [device evidence](../../native/metal-proof/evidence/gpu-matrix-elementwise-2026-10-16.json) |
| Tensor division and scalar lifting | GPU | **Supported.** IEEE division, including either scalar operand order | [source](../../native/metal-source/division.k), [device evidence](../../native/metal-proof/evidence/gpu-division-2026-10-16.json) |
| Tensor unary signs | GPU | **Supported.** float32 vectors and matrices | [source](../../native/metal-source/tensor-negation.k), [device evidence](../../native/metal-proof/evidence/gpu-tensor-negation-2026-10-16.json) |
| Mixed scalar and tensor parameters | GPU | **Supported.** All ten numeric scalar types alongside tensors or in scalar-only tasks; internal Boolean control values | [source](../../native/metal-source/scalar-entry.k), [device evidence](../../native/metal-proof/evidence/gpu-entry-signatures-2026-10-15.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| High-arity CPU/GPU bridge exports | GPU | **Supported.** 29/40-tensor examples use direct/indirect GPU buffers and bounded checked CPU helpers; compiler/device resource limits still apply | [source](../../native/metal-source/indirect-arguments.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-indirect-arguments-2026-10-16.json) |
| Balanced reduceSum | GPU | **Supported.** All ten types, ranks zero through eight; balanced adjacent-pair tree, resident composition and checked integer additions | [source](../../native/metal-source/matrix-reduction.k), [device evidence](../../native/metal-proof/evidence/gpu-forwarding-2026-10-20.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Matrix multiplication | GPU | **Supported.** All ten types, rank two; checked/rounded products and balanced inner tree, resident composition and empty dimensions | [source](../../native/metal-source/matmul.k), [device evidence](../../native/metal-proof/evidence/gpu-forwarding-2026-10-20.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Composed numerical dispatches | Host | **Supported.** Separate GPU calls for matmul, elementwise adjustment and reduction; intermediates remain resident | [source](../../native/metal-source/matrix-pipeline.k), [device evidence](../../native/metal-proof/evidence/gpu-forwarding-2026-10-20.json) |
| Primitive forwarding and re-exports | GPU | **Supported.** Aliases and transitive numerical helpers retain selected invocation shape obligations; multiple numerical stages are supported | [source](../../native/metal-source/forwarding/main.k), [device evidence](../../native/metal-proof/evidence/gpu-forwarding-2026-10-20.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Relative modules and re-exports | Host | **Supported.** Numerical helpers and coordinators retain declaring-file scope | [source](../../native/metal-source/modules/main.k), [device evidence](../../native/metal-proof/evidence/gpu-local-modules-2026-10-20.json) |
| CPU workers inside a Metal program | Host | **Supported.** All ten numeric scalar/tensor bridges, retained CPU worker results and explicit transfers | [source](../../native/metal-source/cpu-spawn.k), [device evidence](../../native/metal-proof/evidence/cpu-spawn-2026-10-16.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Nested parallel groups | Host | **Supported.** Shared scheduler, explicit residence and scope settlement | [source](../../native/metal-source/nested-parallel.k), [device evidence](../../native/metal-proof/evidence/nested-parallel-2026-10-16.json) |
| TaskError recovery | Host | **Supported.** Host catch/match and group settlement; no shader exception handling | [source](../../native/metal-source/recovery.k), [device evidence](../../native/metal-proof/evidence/recovery-2026-10-16.json) |
| Coordinating helper functions | Host | **Supported.** Statically known helper continuations; no general GPU closure dispatch | [source](../../native/metal-source/resumable-helpers.k), [device evidence](../../native/metal-proof/evidence/resumable-helpers-2026-10-16.json) |
| All ten numeric types: typed GPU maps | GPU | **Supported.** Exact integer/floating identities, checked maps and scalar lifting through rank eight; resident typed graphs and conversions | [source](../../native/metal-source/capabilities/numeric-int8.k) (+8 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-typed-source-2026-10-23.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Higher-rank float32 device tensors | GPU | **Supported.** Ranks zero through eight for typed elementwise arithmetic, reductions, resident feedback and conditional stages; matrix products remain rank two | [source](../../native/metal-source/capabilities/rank-three.k), [device evidence](../../native/metal-proof/evidence/gpu-higher-ranks-2026-10-22.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Scalar-only GPU functions | GPU | **Supported.** All ten numeric scalar types, checked scalar-only GPU tasks, internal comparisons and lazy choices | [source](../../native/metal-source/capabilities/scalar-only.k), [device evidence](../../native/metal-proof/evidence/gpu-typed-source-2026-10-23.json) |
| Elementwise expressions before a reduction | GPU | **Supported.** One float32 elementwise stage and one balanced reduction under one task; resident intermediate; rank one/two and mixed parameters | [source](../../native/metal-source/capabilities/reduce-expression.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-pre-reduction-2026-10-21.json) |
| Arithmetic after a reduction inside one entry | GPU | **Supported.** Strict float32 scalar expression after one reduction, optionally with an elementwise input stage; only the final scalar returns to the host | [source](../../native/metal-source/capabilities/post-reduction.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-post-reduction-2026-10-21.json) |
| Branch-local numerical primitives | GPU | **Supported.** All ten numeric types; selected maps/reductions/products, lazy internal Boolean control and selected helper/local shape checks | [source](../../native/metal-source/capabilities/branch-primitive.k), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Elementwise expressions after matrix multiplication | GPU | **Supported.** One resident matrix stage then strict elementwise arithmetic; matching full-shape tensor and scalar operands; direct/Tier 2 bindings | [source](../../native/metal-source/capabilities/post-matmul.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-post-matmul-2026-10-21.json) |
| Arithmetic transforming matrix operands | GPU | **Supported.** Independent resident elementwise maps of either/both operands, one strict matrix product and optional post arithmetic; direct/Tier 2 bindings | [source](../../native/metal-source/capabilities/pre-matmul.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-pre-matmul-2026-10-21.json) |
| Matrix product consumed by reduction | GPU | **Supported.** One strict matmul followed by one row-major reduceSum under a scalar task; optional input maps, matrix adjustments and final scalar arithmetic have separate qualification; additional primitives remain separate work | [source](../../native/metal-source/capabilities/matmul-reduction.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-matmul-reduction-2026-10-21.json) |
| Arithmetic between matrix product and reduction | GPU | **Supported.** Strict tensor/scalar arithmetic on one matrix result before one row-major reduceSum; optional input maps; projected direct/Tier 2 bindings; one scalar task | [source](../../native/metal-source/capabilities/matmul-adjust-reduction.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-matmul-adjust-reduction-2026-10-21.json) |
| Scalar arithmetic after a composed matrix reduction | GPU | **Supported.** Strict float32 scalar helpers/arithmetic after the matrix/reduction pair, including empty sums; independent owned captures; optional input maps and matrix adjustment; only final scalar readback | [source](../../native/metal-source/capabilities/matmul-scalar-reduction.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-matmul-scalar-reduction-2026-10-21.json) |
| Multiple matrix products in one task | GPU | **Supported.** Rank-two float32 product chains and shared intermediates; arithmetic and final reduction have separate qualification | [source](../../native/metal-source/matrix-graph.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-matrix-graphs-2026-10-21.json) |
| Reduction and scalar arithmetic after a matrix graph | GPU | **Supported.** One final strict row-major reduceSum after multiple products; optional float32 scalar helpers/arithmetic before the only scalar readback | [source](../../native/metal-source/matrix-graph-reduction.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-graph-reduction-2026-10-21.json) |
| Tensor arithmetic within matrix graphs | GPU | **Supported.** Strict rank-two float32 maps before/between/after products, shared operands, scalar lifting and one optional final reduction; each map binds only its live operands | [source](../../native/metal-source/matrix-graph-arithmetic.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-graph-arithmetic-2026-10-21.json) |
| Multiple resident reductions and scalar joins | GPU | **Supported.** Rank-one/rank-two float32 reductions over inputs and shared intermediates; final strict scalar expression and one readback; larger joins use Tier 2 | [source](../../native/metal-source/multiple-reductions.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-multiple-reductions-2026-10-21.json) |
| Reduced scalars feeding tensor arithmetic | GPU | **Supported.** Straight-line rank-one/rank-two float32 graphs returning a scalar; device scalar broadcasts, repeated feedback and matrix composition | [source](../../native/metal-source/reduction-feedback.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-reduction-feedback-2026-10-21.json) |
| Resident tensor results after reduction feedback | GPU | **Supported.** Rank-one/rank-two float32 tensor results with one or more contributing reductions; no final scalar shader/readback; explicit transfer or resident GPU forwarding | [source](../../native/metal-source/tensor-feedback.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-tensor-feedback-2026-10-21.json) |
| Scalar conditionals and strict float32 comparisons | GPU | **Supported.** All six IEEE comparisons, internal Boolean logic and pure scalar if arms; bitwise selection preserves float32 values and composes with resident reductions | [source](../../native/metal-source/scalar-control.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-scalar-control-2026-10-21.json) |
| Same-source elementwise tensor choices | GPU | **Supported.** Pure float32 rank-one/rank-two arms over one logical tensor source fuse into one map; predicates may use resident reductions | [source](../../native/metal-source/tensor-control.k) (+1 additional probes), [device evidence](../../native/metal-proof/evidence/gpu-tensor-control-2026-10-21.json) |
| Choices between equal fixed-shape tensor sources | GPU | **Supported.** Pure equal-shape float32 choices retain the fused path; independent/different extents use selected-stage numerical programs | [source](../../native/metal-source/tensor-sources.k), [device evidence](../../native/metal-proof/evidence/gpu-tensor-sources-2026-10-21.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Choices with checked dynamic-shape equality | GPU | **Supported.** Fused choices use unconditional extent proofs; branch-local and different-source obligations execute in selected-stage programs | [source](../../native/metal-source/dynamic-choices.k), [device evidence](../../native/metal-proof/evidence/gpu-dynamic-choices-2026-10-21.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Fused choices with explicit multi-source operand lists | GPU | **Supported.** No fixed two-source cap; owned sorted map operands preserve shape proofs, projected bindings, resident reductions and last-use reclamation; compiler/device budgets still apply | [source](../../native/metal-source/nary-choices.k), [device evidence](../../native/metal-proof/evidence/gpu-nary-choices-2026-10-21.json) |
| Choices in matrix operands | GPU | **Supported.** Independent projected left/right pure tensor choices, fixed or checked dynamic equal shapes; optional result arithmetic and terminal sum | [source](../../native/metal-source/operand-choices.k), [device evidence](../../native/metal-proof/evidence/gpu-operand-choices-2026-10-21.json) |
| Choices over computed matrix results | GPU | **Supported.** One already-computed matrix product feeds pure shape-proven result choices, projected resident epilogues and an optional terminal reduction | [source](../../native/metal-source/result-choices.k), [device evidence](../../native/metal-proof/evidence/gpu-result-choices-2026-10-21.json) |
| Scalar choices after resident reductions | GPU | **Supported.** Scalar choices over resident numerical results support all ten numeric result types, including selected primitive arms | [source](../../native/metal-source/reduction-choices.k), [device evidence](../../native/metal-proof/evidence/gpu-reduction-choices-2026-10-21.json), [device evidence](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json) |
| Host Boolean decisions and conditional numerical dispatch | Host | **Supported.** Checked Boolean helpers, comparisons, lazy logic and selected GPU task dispatch; unselected branch shape checks are skipped | [source](../../native/metal-source/host-control.k), [device evidence](../../native/metal-proof/evidence/gpu-host-control-2026-10-22.json) |

<!-- capability-matrix:end -->

## How this table is checked

The machine-readable contract is
[`capabilities.json`](../../native/metal-source/capabilities.json). Its source
probes must first pass the shared placement checker. Supported probes must then
emit host, continuation, placement and kernel artifacts with a consistent kernel
manifest. Unsupported probes must produce a GPU admission diagnostic, preserve
existing output bytes, and leave a fresh output path absent.

The table is generated from that contract. A stale table fails the test. To
regenerate it after deliberately changing a capability and its probes:

```sh
python3 native/metal-source/CapabilityTests.py --write-docs
```

These matrix checks stop at source emission: they do not execute shaders or
establish numerical correctness, performance or runtime lifetime safety. The
separately linked device evidence records the corresponding hardware and failure
qualification. Some older evidence uses the former source extension or LLVM
version; those records are historical, while the source-admission probes run
against the current compiler. [Installed numerical library acceptance](native-metal-install.md#installed-numerical-libraries)
adds current LLVM 23 Debug/Release bundle execution on the M2 Max, including
CPU/GPU private dependency versions and all four generated host modes.

The [initial matrix audit](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-capability-matrix-2026-10-20.json)
records the pre-repair 32-probe audit, including the two high-arity positive
gates that failed at that point. The repair record below covers current acceptance.

## GPU-HOST-001: repaired high-arity source admission

The older `arguments.k` and `indirect-arguments.k` full-program examples had begun
rejecting with `G001: function exceeds the conservative native stack budget`.
Their signatures and placement remained valid, but their CPU bridge contained
redundant tensor descriptor SSA and long inlined elementwise bodies.

[Bounded tensor helpers and checked IR simplification](native-stack-admission.md)
restore those examples without raising the 16 KiB frame budget or exempting
source functions from admission. The matrix now requires positive source emission
for both, and their positive GPU CLI gates remain required. The older audit record
above describes the failure before this repair; the
[repair qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/gpu-host-stack-repair-2026-10-20.json)
records current acceptance and retained safety checks.

## Remaining N2 work

[Elementwise expressions before one reduction](native-gpu-pre-reduction.md),
such as `reduceSum(values + 1)`, now lower to two resident GPU stages under one
task. [Scalar arithmetic after that reduction](native-gpu-post-reduction.md)
also stays on the GPU, including empty inputs. [Elementwise matrix results](native-gpu-post-matmul.md),
such as `matmul(input, weights) + bias`, now use two resident stages under one
tensor task. [Matrix operand arithmetic](native-gpu-pre-matmul.md) now adds
independent maps of either/both inputs with an optional post stage. Composition
now also includes [one matrix product consumed by one reduction](native-gpu-matmul-reduction.md), returning a scalar task. [Arithmetic on the matrix result before reduction](native-gpu-matmul-adjust-reduction.md) now stays resident too. [Final scalar arithmetic after the pair](native-gpu-matmul-scalar-reduction.md) now stays on GPU too, including empty sums. [Product-only matrix graphs](native-gpu-matrix-graphs.md) now support chains and shared intermediate matrices in one tensor task. [One final reduction and scalar arithmetic](native-gpu-graph-reduction.md) now consume those graphs within one scalar task. [Tensor arithmetic within those graphs](native-gpu-graph-arithmetic.md) is now supported. [Multiple reductions](native-gpu-multiple-reductions.md) now feed one final scalar expression. [Reduction feedback](native-gpu-reduction-feedback.md) now feeds tensor stages within scalar-returning graphs. [Tensor results after feedback](native-gpu-tensor-feedback.md) now remain resident too. [Pure scalar choices and strict comparisons](native-gpu-scalar-control.md) now run on GPU too. [Same-source elementwise tensor choices](native-gpu-tensor-control.md) now fuse pure arms into one map. [Two-source equal-fixed-shape choices](native-gpu-tensor-sources.md) now compose with those graphs. [Checked dynamic-shape choices](native-gpu-dynamic-choices.md) now reuse existing unconditional contracts. [Multi-source fused maps](native-gpu-nary-choices.md) now carry owned operand lists beyond two sources. [Choices over computed matrix results](native-gpu-result-choices.md) now use the resident result-map path. [Scalar choices after reductions](native-gpu-reduction-choices.md) now stay resident for single sums and matrix-product sums too. The wider-type path now admits all ten numeric types for checked maps and scalar-only tasks, including rank-zero sums and rank-eight maps. Basic typed matrix products and resident matrix-to-sum stages have source/device coverage. Wider-type graph composition, unproven different-source shapes, conditional primitive stages and heterogeneous conversion integration remain open. The integer-only conversion algorithms have independent qualification for all 100 type pairs. The [completion checklist](native-gpu-completion.md) tracks the remaining work. Host closures and TaskError handlers remain coordinating operations.

Custom user kernels/shaders, lower-level device interfaces and NVIDIA/CUDA remain
later work in their respective plans. This matrix does not add release
requirements beyond the accepted numerical GPU scope.
