# Shared placement-aware typed IR

The shared native frontend now represents device selection, explicit transfers, placed calls, and tensor residency. The optional Metal source target must pass this check before publishing artifacts. The mixed CPU/GPU supervisor now lowers this checked representation into resumable native C++ continuations. It no longer walks expression syntax or resolves local names a second time.

## Checked representation

`ValueType` retains the existing numeric/shape/task/error information and adds an exact optional GPU index. For a tensor, an absent index means host residence. For a `Device` value, it means the CPU selector. GPU zero is distinct from an absent index. Numeric scalars stay host values, including completed GPU reductions.

- `DeviceSelection` produces a CPU or GPU selector. GPU indices are parsed directly as uint32 constants, never rounded through float32. Immutable aliases keep the same selection.
- `Transfer` consumes a completed tensor and a selector, and produces an owned task with the target residency and the original rank/extent facts.
- `TaskOperation` carries a checked target value for a placed spawn. In placement mode, omitted placement becomes an explicit CPU selection. Arguments precede selector evaluation and admission. Await/try preserve tensor residence.
- The existing parallel, catch, match, failure and shape operations retain their control-flow and ownership obligations. No new scheduler or lifetime mechanism is introduced.

A function definition describes its ordinary CPU interface and checked tensor algorithm. A GPU spawn refers to that same function with an explicit execution target; selecting a target does not turn the CPU expressions which compute its arguments into GPU code. Backend capability checks still decide whether that algorithm can be compiled for a particular device.

Host arithmetic, ordinary CPU calls and CPU task captures reject resident GPU tensors. GPU tensor captures must already reside on the selected GPU. Scalars may be captured by either target. Function results must be downloaded to the host under the current interface. Catch/match success branches must agree on residency and GPU index; no branch inserts a transfer. Device selectors cannot escape as numeric/group/function results.

Local operations retain proven shape facts. In placement mode, CPU calls expose their declared result shape contract at the ABI boundary. A declared dynamic result remains dynamic rather than gaining a stronger promise from its current implementation. Known invalid arguments still fail checking. Unknown fixed-parameter constraints on spawned CPU functions remain obligations of the child invocation, so a shape fault can be handled through its settled parallel group. Ordinary direct calls retain their supervisor-side refinements.

The representation is hardware-independent: checking `devices.gpu(1)` does not require a second GPU. Runtime availability, device capabilities, quotas, reflected bindings and kernel suitability are separate checks. In particular, the IR can describe an explicit cross-device transfer while the current Metal backend continues to reject unsupported cross-GPU execution.

## Inspecting the IR

```sh
build/native/native/kalandra-check --placement --dump-ir \
  native/metal-source/nested-parallel.k
```

The ordinary checker invocation keeps its CPU policy; placement must be enabled explicitly. CPU lowering rejects a module containing device selectors, transfers, placed calls or resident types rather than silently dropping them. Ordinary CPU source and lowering remain available through the existing entry point.

Metal compilation also writes `placement.ir` alongside `host.cpp`, CPU MLIR/LLVM and GPU artifacts. That file must exactly match the standalone shared check of the original source. Sanitizer/optimization flags do not change it. CMake tracks it as a generated output, and it participates in the compiler's source/output collision and symlink preflight. It is an inspection artifact, not a stable serialized module or a supported input to the compiler.

## Explicit literal shapes

The common frontend also supports the accepted explicit-shape constructor for **flat literal data and constant dimensions**:

```kalandra
empty: Tensor[float32, 0, 3] = tensor([], [0, 3])
matrix: Tensor[float32, 2, 3] = tensor([1, 2, 3, 4, 5, 6], [2, 3])
```

`tensor([])` remains a rank-one empty tensor. An annotation cannot silently turn it into a matrix. The explicit shape supplies the rank and extents; its checked element count must equal the flat data length. Dimensions are nonnegative integer constants parsed without float conversion. Invalid dimensions, indexing/allocation overflow and unrepresentable row-major strides fail before lowering, including zero-element shapes whose nonzero suffix would overflow a stride. Numeric elements still need float32 context in this implementation.

The result uses the existing `TensorLiteral` IR and CPU tensor lowering. The Metal supervisor supports its current rank-one/rank-two constant-data subset. General runtime arrays, computed dimensions, checked ShapeError constructors and additional element types remain later work. Unconstrained numeric bindings continue to follow the accepted numeric design; the float32 subset needs an annotation when no expression/parameter/result context supplies that type.

## Qualification and remaining work

[Placement frontend tests](../../native/tests/PlacementTests.cpp) check all 19 mixed source fixtures, typed dependencies, selector aliasing and exact uint32 indices, capture/residency rules, task/error obligations, fixed-shape child boundaries, constant shapes, overflow and analysis limits. They run normally and with ASan/UBSan, without a GPU. They also verify that the CPU backend rejects placement-bearing modules and continues to lower ordinary CPU modules.

[Artifact checks](../../native/metal-source/VerifyPlacement.cmake) compare 70 generated target artifacts with independent standalone checks, check option ordering, and verify output preflight. [CPU constructor checks](../../native/cmake/VerifyExplicitShapes.cmake) execute flat matrices, empty rows/columns, a rank-zero tensor and a returned empty tensor in debug, optimized, ASan/UBSan and TSan builds. Existing mixed CLI suites now use explicit shapes for zero outer dimensions and retain their worker, numeric, failure and sanitizer checks. [Evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/placement-ir-2026-10-16.json) records the regression and unchanged lowering artifacts.

## IR-driven supervisor continuations

The Metal supervisor consumes `TypedStatement` bodies and typed expression nodes for values, calls, selectors, transfers, spawn/await/try, shape checks, parallel groups, catch, exhaustive matches and error fields. Resolved `SymbolId` bindings replace string-based local lookup. Calls map checked function IDs to the CPU export table explicitly, so placing `main` before helper definitions cannot change which function is invoked.

A path-local value map emits shared graph nodes once. Each conditional arm starts with its dominating values; branch-local values are not reused after the branch. In particular, reading pattern fields uses the already evaluated match subject rather than repeating an awaited computation or opening its group again. Nested scopes keep explicit open/close/join continuations, and failed `try` paths still exit through settlement before a handler runs. Shape-check nodes guard completed successful values; an expected-error result does not turn into a shape fault while awaiting recovery.

Literal rounding and tensor dimensions come from the shared checker. Tensor unary plus is already an identity in that IR, so the supervisor no longer generates its former redundant CPU wrapper. The supervisor preserves exact signed literal bits and reuses the existing CPU numeric kernels and Metal task admissions. CPU argument evaluation still precedes target selection and task admission. Pipelines use their resolved `FunctionCall` nodes, including ordinary CPU calls in mixed programs.

The [integration fixture](../../native/metal-source/ir-supervisor.k) combines a main-first declaration order, CPU pipelines/tasks, GPU work, nested shadowing, pattern fields, a recovered child shape fault, explicit shape guards and a returned tensor. It runs with 1/2/4 workers in debug, optimized, ASan/UBSan and TSan builds. A sanitizer-instrumented compiler must produce identical host, CPU, GPU and placement artifacts. Existing failure injection, cancellation, nested task and recovery suites also qualify the replacement. [Continuation evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/ir-supervisor-2026-10-16.json) records those checks.

The generated supervisor remains a target-specific C++ backend; it and ordinary CPU task lowering now consume the [common continuation planner](continuations.md). Function signature capability checks and CPU export construction retain their source-level interface. Resumable mixed helper bodies, broader value/element types and additional devices remain separate increments. Distributed execution retains its design-review gate.
