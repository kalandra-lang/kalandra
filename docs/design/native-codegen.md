# Native CPU source compilation

**Current execution policy:** [Hardware arithmetic and explicit software fallback](hardware-arithmetic.md) is the default policy as of 2026-10-24. CPU float32/float64 and Metal float32 use hardware arithmetic. Metal float64 arithmetic requires explicit fallback permission. Exact-bit software proofs below describe the opt-in software implementation, not the default hardware path.

Implemented first source-to-native path — 2026-10-15. The C++23 compiler now lowers the [checked IR](semantics.md) through MLIR's native C++ API, bufferization, ownership/deallocation, explicit loops, LLVM translation, and native linking. The [projection source](fixtures/cpu-projection.k) builds into an executable that prints 163.

This is the float32 CPU subset on macOS ARM64. It includes the [source task subset](native-tasks.md#compiled-source-task-subset) on a bounded multi-worker CPU core. Devices, remote execution, general arrays/records, integer arithmetic, and the final application IO/runtime APIs remain pending.

## Compile and run

Prerequisites are macOS ARM64 with the Apple command-line SDK, CMake 3.28 or newer, Ninja, and the exact LLVM/MLIR/Clang **23.1.3** release in the [toolchain lock](../../native/toolchain-proof/toolchain.json). The validated installations are CMake 4.4.2 and Ninja 1.13.2. The default LLVM prefix is `/opt/homebrew/Cellar/llvm/23.1.3_2`; an installation elsewhere must be selected with `-DKALANDRA_LLVM_ROOT=/path/to/llvm-23.1.3` during configuration. LLVM headers, libraries, CMake packages, mlir-opt, and mlir-translate must all come from that release. An unversioned Homebrew upgrade is not a substitute for the pinned release.

From the repository root:

```sh
cmake --preset native
cmake --build --preset native
build/native/native/kalandra docs/design/fixtures/cpu-projection.k \
  -o build/native/projection
build/native/projection
ctest --preset native
```

The executable prints `163` and a newline. Its application dependencies are libc++ and the macOS system library; it does not load LLVM, MLIR, Bun, Node, or a JavaScript engine.

The development entry convention is a parameterless main returning float32. The linked wrapper prints that value with round-trip precision. This is a milestone harness, not a finalized language printing/exit-code API.

Compiler options:

| Option                  | Behavior                                                                                                |
| ----------------------- | ------------------------------------------------------------------------------------------------------- |
| `-o path`               | Required executable destination                                                                         |
| `--emit-mlir path`      | Save generated, verified tensor-level MLIR                                                              |
| `--emit-llvm path`      | Save translated, verified LLVM IR                                                                       |
| `-O0` / `-O2`           | Native optimization setting; default O0; both preserve strict numerical policy                          |
| `--sanitize`            | Instrument generated code/runtime with AddressSanitizer and available UndefinedBehaviorSanitizer checks |
| `--test-entry function` | Add a private C-interface wrapper for runtime-input tests; not a public FFI                             |

The compiler invokes the pinned Clang directly with an argument vector, using a unique temporary LLVM file and executable output. It publishes the executable only after successful linking. Source/output collisions are rejected, and invalid source leaves an existing executable destination unchanged. Saved IR files are explicit diagnostic artifacts and may be written before linking succeeds.

The compiler currently expects its CMake-built runtime archives at their build locations. Installing or distributing the compiler is subsequent packaging work; this is reproducible development tooling from a built checkout.

## MLIR lowering

User functions get compiler-owned native symbols. Parameters and result types are built from checked types; compatible tensor casts reconcile precise local shape facts with declared function signatures. Ordered statement roots and operands preserve once-only, left-to-right evaluation. Compiler-owned source locations accompany generated operations.

The verified pipeline is:

1. Build tensor/arith/func/scf/memref/vector operations through C++ APIs and verify the module.
2. Bufferize function boundaries, then lower linalg operations introduced by tensor generation to loops.
3. Insert ownership-based buffer deallocation and convert remaining bufferization operations to memref operations.
4. Lower memref copies to explicit rank-aware loops, including copies required when an input tensor is returned as an independently owned result.
5. Lower vector operations, structured control flow, memory, arithmetic, and function operations into LLVM dialect operations; reconcile conversions.
6. Translate into LLVM IR, apply the checked invocation ABI to every generated function and allocation/free/fault site, verify the resulting module, and link it with the minimal native runtime.

Matrix multiplication allocates one reduction scratch buffer per operation and reuses it across serial row/column tiles, contraction blocks, and edge columns. The tensor carried through those loops bufferizes in place. Scratch is local to the invocation, never shared between concurrent tasks or stored in a global/thread-local cache. Products are still separately rounded, and each cell uses the same balanced adjacent reduction tree; ordinary `reduceSum` keeps its own scratch. For empty output shapes, scratch has zero length and the row loop is skipped, including when an unused extent is extremely large. Ownership deallocation and the checked ABI settle these buffers on normal return, allocation/shape faults, and cooperative cancellation.

Intermediate ownership conventions may copy an input returned by a function. That is valid immutable behavior; removing unnecessary copies is later optimization work. Returning an input and using both its original and returned values is covered by an executable regression test.

Source functions carry noinline in emitted LLVM so downstream optimization preserves the compiler's checked stack boundaries. Before emission, [budgeted checked-call inlining](checked-call-optimization.md#budgeted-checked-call-inlining) may expand one level of small scalar calls while retaining logical admission/polls and rechecking the combined stack budget. Tensor/aggregate signatures keep their call boundaries. The backend initially registers the stock dialect/extension/pass set required by the pinned toolchain; narrowing compiler registration and deployment size is also later work.

## Internal CPU ABI and ownership

This ABI is private to the pinned compiler/toolchain. It is not a stable external contract.

- Each generated function receives an explicit invocation-context pointer as its final parameter, including the private C wrappers. Scalars are LLVM float values. Tensor function arguments/results use MLIR's ranked memref descriptor convention after bufferization: allocated and aligned pointers, an element offset, then signed 64-bit sizes and strides for each dimension. A scalar result stays a scalar.
- Source tensors are immutable. A callee borrows input storage during the call and does not release caller-owned input buffers. Passing one source value twice, binding another name to it, and retaining it after a call are valid.
- A returned tensor must outlive the callee. Bufferization may transfer newly allocated backing storage or insert a copy when returning borrowed input; ownership lowering determines the eventual release. Callers must not assume pointer identity or a particular copy count.
- Scratch storage belongs to the generated operation and is released after use. A fault stops the current call chain through checked status; the invocation boundary settles its own outstanding owners, including earlier tensor results and nested-call buffers. Other invocations retain their own storage.
- The private C test wrapper accepts ranked descriptors by pointer plus the final invocation context. A scalar result returns float32; a tensor result writes a ranked descriptor through an additional first output pointer. The caller must check invocation fault/cancellation before reading a result, and must keep the invocation alive while reading returned storage. A failure return is not a successful value. Its inputs have independently owned, contiguous, row-major storage for the duration of the call. The wrapper validates dimensions/counts, but the harness must guarantee valid backing pointers and storage capacities. Language-level repeated arguments are tested inside Kalandra functions, without violating the wrapper's independent-input promise.

The compiler verifies MLIR and LLVM IR against their internal types. These checks do not turn an arbitrary external descriptor into safe input. Public FFI validation, stable ABI versioning, and non-contiguous external views remain separate work. Local task ownership across workers follows the separate task contract.

## Strict arithmetic and reductions

Float32 literals use the exact bits from semantic analysis. Scalar operations have no fast-math flags. Native linking disables floating contraction; tests also inspect optimized assembly for unexpected fused multiply-add instructions.

Matmul generates each output element from separately rounded products. A scratch buffer holds the products, then sequential loops combine adjacent pairs, carrying an odd tail unchanged. ReduceSum flattens logical indices in row-major order and uses the same balanced tree. Nonempty reductions introduce no extra zero; empty reductions return additive positive zero. Rank-zero tensors reduce their single element.

This is a correctness baseline. Allocating per-dot-product scratch is intentionally simple and is not competitive tuned matrix code. Performance claims, fusion, vectorization, scratch reuse, and relaxed numerical policies require separate measured work.

## Shapes and memory

Generated guards check unresolved inner dimensions and dynamic-to-static refinements before their affected operations. Dimension/product checks enforce supported indexing and byte-count ranges before allocations. Empty dimensions are handled without overflowing irrelevant products or reading empty buffers. A zero-sized output does not execute or allocate per-element dot-product scratch.

MLIR's ownership/deallocation pipeline determines normal releases. The runtime now uses explicit `Invocation` contexts with independent ownership ledgers, a latched fault code/source location, allocation-failure injection, and deterministic settlement. There is no process-global or thread-local current invocation. A context is accessed exclusively; it can later migrate with its continuation rather than belonging to a worker identity.

A backing allocation has a reference count and an embedded initial owner; additional retained owners have their own ledger entries. `retainFrom` establishes a dependency in another context while the source owner is valid. Releasing or faulting one context drops only its owners. A retained tensor backing can survive its creating context and be released by its final owner. Reference counts use atomic operations; context-ledger access and publication still require the future scheduler's synchronization. This is the acyclic scalar/tensor foundation, not a general cycle collector or a claim that arbitrary runtime operations are thread-safe.

The compiler applies the private checked ABI after LLVM translation, covering allocations introduced by bufferization and ownership-generated clones as well as explicit scratch. Every generated callee receives the context; callers check its latched status before using results. Allocation hooks record Allocation faults and return null only to generated code which immediately branches to the failure return path. Guard sites record their source location and return through the call chain. Failures do not enter subsequent source operations or publish successful values. The failure return's zero placeholder has no source value semantics.

Allocation, release, fault recording, and status use revisioned `kalandra_*_v1` symbols. The old context-free allocation hooks and process-exiting fault symbol are absent from final source-generated LLVM IR. Unrecognized external and indirect calls are rejected by the checked-ABI pass rather than assumed safe. Saved tensor MLIR is an earlier debugging artifact; its fault calls are rewritten in final LLVM IR.

No C++ exceptions or longjmp cross generated frames. Runtime context operations are nonthrowing. Fault recording and settlement do not allocate diagnostic strings. Context failure is sticky, first-fault metadata is retained, and a settled context cannot be re-entered for execution. Allocation/retain construction failure leaves earlier owners available for settlement.

The CLI entry and private command-line test harness remain reporting boundaries: they settle a failed invocation, report ShapeFault (exit 87) or allocation/internal runtime faults (exit 86), and exit. The generated call itself returns a failed status without exiting the process. An embedding supervisor can instead inspect and settle that context while other contexts continue. This is checked-fault containment, not containment of unsafe crashes or corrupted native memory.

Private test hooks can fail an allocation/retained-owner construction at a selected attempt and report cleanup counters. Counters count established/released ownership entries; they are not global process-allocation or retained-byte measurements. Input descriptor wrappers check dimensions and size requirements, but raw-pointer ownership/aliasing must be established by the trusted native harness. Arbitrary foreign pointers or forged storage capacities are outside a public memory-safe FFI claim.

Source parallel/spawn/await now lower to typed LLVM continuations on the [single-worker task core](native-tasks.md#compiled-source-task-subset). TaskError propagation, cached outcomes, bounded admission, tensor handoff, and nested group settlement have source executable tests. CPU kernels can now run through a bounded worker pool; entry/call/loop polls acknowledge cancellation without changing strict arithmetic order. See the [multi-worker contract and evidence](native-tasks.md#bounded-multi-worker-cpu-execution).

## Verification

The 25-test CTest suite retains frontend and handwritten toolchain evidence and adds three source-compilation suites: debug, optimized, and sanitized. Each compiles the actual Kalandra fixture rather than substituting hand-written MLIR.

Each source suite verifies:

- Standalone output 163 with JS/compiler tool directories absent from runtime PATH and no compiler/JS runtime linked.
- 49 runtime-content datasets against the independent C++ float32 reference, including rounding-sensitive products and reductions, signed zero, subnormals, infinities, and NaNs. NaN payload identity is not promised.
- 10,000 calls through the source-compiled reference function, preserving input contents and balancing generated allocations/releases. The current canonical runtime-input path performs 30,000 allocations and releases across that run after matrix scratch reuse.
- Failure injection at every allocation position in one runtime-input invocation, including failures after earlier allocations succeeded, with zero tracked allocations remaining.
- 1,617 runtime shape/content combinations: rows in {0, 1, 2, 3, 5, 8, 17}, columns in {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 17}, contraction lengths in {0, 1, 2, 3, 7, 16, 33}, each with sequential values, seeded finite values, and seeded IEEE edge mixtures. Each case runs twice, compares bits against an independent host reference, preserves inputs, and leaves no tracked storage live. NaN payload identity is excluded. A separate zero-sized case retains an otherwise very large valid dimension.
- Invalid inner dimensions, negative/oversized descriptors, and a shape refinement that fails after real projection allocation, all with cleanup.
- Repeated immutable arguments, local aliases, nested identity/tensor-return calls, and two retained matrix results. This program runs 1,000 times, with failure injection at every allocation position in a separate invocation and complete cleanup each time.
- Fault isolation after real source-compiled allocation: retained parent inputs survive child settlement, allocation failures at every generated position leave no child owners, and a fresh invocation returns 163 in the same process.
- Left-to-right call-argument evaluation, using two independently failing shape refinements and checking the first fault's source location.
- Scalar arithmetic, rank-zero/one/three and empty reductions, preservation of a singleton negative zero, invalid-source diagnostics, and artifact preservation.

Three dedicated invocation-runtime suites additionally compile both tests and runtime in debug, optimized, and ASan/UBSan variants. They exercise 1,000 parent/child/sibling/result handoffs, fault latching, 10,000 duplicate retains with iterative cleanup, each allocation/retained-owner failure position, size overflow, zero-byte backing, invalid owner release, and rejection of settled-context execution. These tests keep several contexts alive simultaneously; they do not imply a concurrent scheduler exists.

AddressSanitizer instruments generated memory accesses and the runtime. UndefinedBehaviorSanitizer instruments runtime/harness C++ and the checks available during LLVM IR compilation; it is not a substitute for explicit overflow validation in already-lowered integer IR. Allocation counters cover the tracked generated buffers, not every process allocation. These tests are evidence for this subset, not a proof of future resource, GPU, or concurrent runtime safety.

## CPU conformance status

The conformance sweep passes in debug, optimized, and sanitized builds. No additional backend defect was exposed by this expanded corpus. The first sequential float32 CPU milestone is complete within its declared scope; this does not mean that all accepted numeric types or the full language are implemented.

A fresh source-only copy also configures, builds, and passes the full 16-test suite with PATH restricted to `/usr/bin:/bin`, CMake/Ninja selected by absolute path, and the pinned toolchain already installed. No cached compiler outputs, prototype packages, Bun, or Node are used. This verifies a clean development build on the current host, not dependency installation on a blank machine. Automated toolchain provisioning, CI on another host, compiler distribution, and performance tuning remain tooling work.

The [native local-task contract](native-tasks.md) now defines the next ABI/runtime direction. Checked invocation contexts and managed tensor ownership now implement its first gate. The single-worker core now passes its synthetic-entry tests. Source task typing/ownership checks and continuation lowering now pass their executable suites. Native TaskError catch/match recovery, field inspection, handler-local propagation, and failure conformance now pass their compiled suites. Multiple-worker synchronization and generated loop polls now pass their runtime/source conformance, including separate ThreadSanitizer runs. The [CPU scaling benchmark](../../native/benchmarks/README.md) now records generated-kernel scaling and scheduling overhead. Matrix reduction scratch now reuses one invocation-local buffer across serial output cells; the [before/after comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cpu-2026-10-15-scratch.md) records allocation counts and throughput. The [scheduler profile](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15.md) now records phase timings, independent record/group-capacity controls, and separate probe observations. The [idle-wakeup comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-wakeups.md) records the implemented notification/empty-drain optimization and renewed numerical-kernel checks. The [targeted-wakeup comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-targeted.md) now records separate worker/controller wait channels and fewer active-phase broadcasts, with renewed numerical checks. The [bounded runnable tracking comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-runnable.md) now records explicit ready events and deferred continuation tracking. The [maintenance comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-maintenance.md) now records bounded occupancy indexing for unfinished tasks and live records/groups, preserving waits and cleanup. The [task-maintenance fast path](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-scheduler-2026-10-15-dense.md) now skips unnecessary primary task scans while keeping group settlement active. The [four-column SIMD comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cpu-2026-10-15-simd.md) now records the first CPU microtile optimization under those arithmetic, cancellation, and ownership gates. Broader cache blocking and target-specific kernel selection remain separate measured work. GPU and remote execution follow their separate capability and deployment gates.

Sanitizer builds mark the generated LLVM functions explicitly: `--sanitize` enables AddressSanitizer memory checks and the ASan/UBSan runtime; `--thread-sanitize` selects ThreadSanitizer attributes and a separate instrumented runtime. The CLI rejects combining them. The compiled-worker suites inspect post-Clang LLVM for real memory instrumentation. Merely passing sanitizer flags when linking unmarked LLVM functions does not instrument those functions. General compiler-process sanitization remains separate from instrumenting its output.

## CPU tiles and bounded contraction scratch

Matrices with at least four rows and sixteen output columns dispatch to a private, non-inlined MLIR kernel. Float32 uses 2×8 output tiles and float64 uses 1×8 tiles, bounding register pressure. Remaining rows use 1×8 tiles; remaining columns use 4/2/1 lanes. Smaller shapes retain scalar/pair/four-column row kernels. A proven unit RHS column stride and at least 32 rows/columns with contraction length 128 admit invocation-local packing of eight-column panels; dynamic row strides, offsets and reversed rows retain their descriptor meaning.

Each output uses separately rounded products and the original balanced adjacent-pair tree. Aligned 32-leaf float32 and 16-leaf float64 subtrees are formed in SSA/registers and only their roots are written to scratch. Partial groups carry odd leaves unchanged without padding. No reassociation, fast math or fused multiply-add is introduced. Generic `reduceSum` remains unchanged.

One invocation-local scratch allocation is reused across tiles and edges. Its wide payload bound is `elementBytes * tileLanes * (B + 1 + floor(K/B))`, with 16 tile lanes for float32 and eight for float64. Here `B` is 32 for float32 and 16 for float64. This includes a disjoint `B`-leaf tail workspace and space for complete/partial roots; it is not constant-space reduction. Narrow payload remains `elementBytes * width * K` for widths 1/2/4. Empty outputs use zero scratch and skip rows even when an unused extent is enormous. All payload arithmetic is checked before allocation. Packing adds a separate managed allocation where admitted. Tensor output retains the existing 64 alignment bytes, plus runtime owner metadata outside the generated request. The [matrix follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/matrix-tuning-2026-10-25.md) records the current measurements and qualification.

The [per-cell corpus](../../native/tests/MatrixHarness.cpp) now checks 3,681 general cases and 46 shared-input cases, each repeated, alongside 1,617 aggregate cases. It covers all partial row counts, column panels/tails, K256 boundaries and multiple root levels through K8193, IEEE edges, signed zero, float-aligned inputs with sentinels, and every allocation-failure point for direct/blocked/edge/shared paths. The private tensor-result wrapper enables checks that a final sum could hide. Wrapper inputs remain independently owned; shared arguments occur inside Kalandra functions.

An [untimed forwarding probe](../../native/tests/MatrixAllocationProbe.cpp) validates actual generated allocation requests for 506 shapes, including unused extreme dimensions. It verifies one product/root allocation plus one tensor output, byte sizes, and cleanup. Debug/optimized/generated ASan/UBSan conformance retains strict vector arithmetic and ARM64 assembly checks. Worker tests run full and partial tiles concurrently, and exercise in-loop cancellation/deadlines for widths 1/2/3/6/7/16/17/18/19/31/32/33/256, with separate TSan coverage. Loop polls remain bounded by generated iterations, not a constant number of scalar operations or a wall-clock deadline.

See the [matched LLVM before/after report](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-mlir-blocking-2026-10-15.md) for performance and scratch tradeoffs. This is an initial strict CPU tile, not a BLAS dispatch or GPU backend.

## Shape measurements and next kernel work

The [37-shape comparison](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-shapes-2026-10-15.md) rebuilds scalar and SIMD LLVM against identical workload/runtime code. Rectangular/wide/deep shapes retain the SIMD gain, but scalar tails can make seven columns slower than eight. Tall one-column and three-column cases show smaller regressions. An untimed probe finds one extra generated-loop poll per row when no full four-column tile exists; group/worker scheduling is unchanged.

Shape-aware narrow dispatch and two-lane output/tail kernels are now implemented. The [paired follow-up](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-shapes-2026-10-15-pairs.md) compares them with the four-lane-only build using the same shape suite. The adjacent-pair tree, scalar remainder semantics, and bounded invocation-local allocation remain intact. The [MLIR blocking port](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-mlir-blocking-2026-10-15.md) now implements the subsequent 4×16/K256 path with separate narrow kernels.

## Cache-layout evaluation

The [cache-blocking research](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-cache-2026-10-15.md) evaluates larger rectangles with unchanged generated code, strict C++ prototypes, and a single-threaded Accelerate reference. Power-of-two K blocks preserve the adjacent-pair tree by forming aligned subtrees and then reducing their roots; this is distinct from independently summing arbitrary K chunks or using a running accumulator. Four-row/sixteen-column tiles reuse right-hand values across rows. The evaluation includes unblocked controls for the same layout, byte-footprint calculations, block-boundary/IEEE conformance, and sanitizer checks. Those research kernels remain separate from production lowering and add no vendor dependency to native builds. Their layout now informs the [MLIR implementation and validation](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/benchmarks/results/native-mlir-blocking-2026-10-15.md), retaining ownership, size guards, cancellation, and numerical gates.

## Elementwise tensor arithmetic

The shared TensorBinary operation lowers float32 `+`, `-`, `*` and `/` over ranked tensors to fresh result storage and serial MLIR loops. Scalar operands lift over tensor elements; two tensor operands require equal ranks and all corresponding extents. Dynamic shape guards and overflow-safe element counts run before result allocation/computation. Loops use the existing checked invocation/cancellation protocol and ownership-based deallocation. The task backend extracts TensorBinary as a compiled kernel, so ordinary CPU tasks execute the same operation. Rank-two, empty-dimension, scalar-lifting, and task cases accompany CPU/GPU numerical conformance; GPU admission remains a separate capability check; the [shared source subset](apple-gpu.md#strict-tensor-subtraction-and-scalar-negation) now includes rank-one/rank-two float32 subtraction and scalar negation.

The shared TensorUnary operation now lowers whole-tensor `-` through the same checked ranked-loop and ownership path. Each lane emits MLIR `arith.negf`, preserving every non-sign bit, rather than subtracting from zero or multiplying by -1. TensorUnary retains its operand during CPU task kernel extraction. Unary `+` remains an identity. [Whole-tensor unary qualification](apple-gpu.md#whole-tensor-unary-arithmetic) covers exact CPU/GPU bits, input immutability, empty dimensions, nested helpers, inline CPU task negation and rank-three CPU lowering.

### Inline operations in mixed CPU/GPU programs

The [mixed-program bridge](apple-gpu.md#inline-cpu-expressions-in-mixed-programs) now routes inline scalar/tensor arithmetic, unary signs, `reduceSum`, and `matmul` through this CPU lowering. Private operation wrappers are checked by the same frontend and imported with fresh typed IDs and original expression spans. This reuses numeric, shape, ownership and cancellation rules rather than emitting host C++ arithmetic. It is an extension of the current target bridge; placement/task/residency IR unification remains separate work. CPU tensor argument export still copies descriptors' payloads, and expression chains are not fused by this adapter.
