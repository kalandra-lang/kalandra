# Native resolution and typed IR

Implemented first CPU semantic subset — 2026-10-15. The handwritten C++23 frontend now resolves symbols, checks calls and tensor shapes, and constructs a typed IR for the [projection fixture](fixtures/cpu-projection.k). This completes the initial semantic gate of the [CPU milestone](cpu-milestone.md). The [native backend](native-codegen.md) now consumes this IR to compile the projection source.

## Inspect the checked program

```sh
cmake --build --preset native
ctest --preset native
build/native/native/kalandra-check docs/design/fixtures/cpu-projection.k
build/native/native/kalandra-check --dump-ir docs/design/fixtures/cpu-projection.k
```

The canonical fixture checks as four functions and 26 typed values. The check command prints source diagnostics and exits nonzero on failure. Dump mode emits no partial IR after an error.

The projection retains a tensor operation rather than becoming scalar loops at this stage:

```text
fn @0 project($0 samples: Tensor[float32, ?d1, 3], $1 weights: Tensor[float32, 3, 2]) -> Tensor[float32, ?d1, 2] {
  %0 : Tensor[float32, ?d1, 3] = read $0
  %1 : Tensor[float32, 3, 2] = read $1
  %2 : Tensor[float32, ?d1, 2] = matmul strict_f32 balanced_adjacent separately_rounded_products %0, %1 check_inner=0 check_allocation=1
  statement %2
  return %2
}
```

Names such as `?d1` are internal extent identities in a debug dump, not additional source syntax. Here the output row count is the input row count. No inner-dimension guard is needed because both contraction extents are statically 3. Output allocation size still depends on runtime rows and must be checked by the eventual lowering before allocation.

## Symbols and function results

Function signatures are registered before checking bodies, so a function can call a later declaration. Parameters and locals resolve to stable symbol IDs; calls resolve to function IDs. Duplicate function/parameter/local names, unknown values/functions, wrong arity, and attempts to call a scalar/tensor are diagnosed.

A local name becomes visible after its initializer. Its binding cannot overwrite a parameter or an earlier local. Function names and builtins may be shadowed by a local value; calling that value does not silently fall back to the hidden function. Ordinary first-class function values and indirect calls are not in this subset.

A function can infer its result from a checked body. Recursive result inference needs an annotation to establish a concrete type; a recursive cycle anchored by a declared result can be checked. There is no Any or guessed numeric fallback. Declared result types and more precise body result facts are retained separately.

## Numeric typing

The first semantic representation supports float32 scalars and ranked float32 tensors. Other accepted language types receive an explicit unsupported-subset diagnostic. In particular, an unconstrained integer literal still requires int64 and an unconstrained decimal requires float64; neither silently becomes float32 because this compiler stage supports only float32.

Parameter types, binding annotations, declared return types, and a known scalar operand can provide float32 literal context. Literal spelling is converted directly to IEEE binary32 through LLVM APFloat with nearest/ties-to-even rounding. The IR stores exact bits. Finite literal overflow is a diagnostic; representable subnormals and underflow rounding to zero remain valid. Unary minus stays explicit, preserving negative zero.

Scalar `+`, `-`, `*`, and `/` remain strict float32 operations. They are not silently fused or reassociated, and are not evaluated by this checker. `%` is rejected in this float32 subset because the accepted operator is integer remainder. Numeric conversions, integer arithmetic, and float64 arithmetic remain subsequent implementation work.

## Tensor literals and shapes

A tensor constructor currently needs explicit Tensor[float32,...] context and one literal argument. Rectangular nested arrays become row-major scalar operands; ragged shape, rank mismatch, and incompatible known extents are diagnosed. General array values and runtime flat-data/shape constructors are not implemented.

The nesting of the literal determines its rank. For example, `tensor([[], []])` can construct a contextual float32 tensor of shape `[2, 0]`. `tensor([])` has shape `[0]`; an annotation cannot silently turn it into `[0, 3]`. Constructing that latter shape from flat data needs the later explicit-shape constructor. Rank-zero scalar construction remains distinct from a scalar result.

Elementwise float32 `+`, `-`, `*` and `/` now produce a TensorBinary IR node when either operand is a tensor. Scalar operands lift across all elements on either side; tensor operands must have identical rank and extents. Tensor-axis broadcasting and implicit numeric conversions are not implemented. Division follows the float32 rounding and exceptional-value rules; zero divisors do not introduce a TaskError. Known shape mismatches fail compilation; dynamic equality requirements are retained in the function summary and checked at runtime by native lowering. Scalar lifting does not allow a scalar argument where a function parameter requires a tensor. Unary `+` preserves a completed tensor value. Unary `-` now produces a shape-preserving TensorUnary operation for completed float32 tensors, with exact sign-bit semantics. It adds no shape equality preconditions; task/fallible values must still be observed explicitly before arithmetic.

Matmul requires two rank-two float32 tensors. Known incompatible inner dimensions fail compilation. Independent `?` extents are distinct facts, so unresolved compatibility records an inner-dimension guard. Output rows and columns retain their relationships to the operands.

A dynamic tensor passed to a fixed-shape parameter or assigned a fixed-shape annotation gets a ShapeCheck IR node. Only the checked value is refined; the source binding retains its original facts. Inferred relationships substitute through ordinary calls, allowing a known input row count to become a known result row count. Unknown result dimensions not tied to parameters get fresh identities per invocation.

Completed functions summarize shape equalities involving parameter extents and constants. Calls reject statically incompatible requirements even through ordinary wrappers, and propagate unresolved parameter requirements. Recursive calls under analysis conservatively retain runtime guards instead of assuming an unfinished summary is complete.

Known dimensions must fit the supported signed 64-bit indexing range. Complete static element/byte-count products are checked for overflow, including zero-sized shapes without overflowing irrelevant products. Unknown products remain runtime validation obligations. Input descriptor validation belongs to the native ABI/lowering stage; a typed signature is not proof that an external descriptor is valid.

## IR and execution obligations

The IR contains explicit float32 literals, symbol reads, scalar operations, elementwise tensor unary/binary operations, tensor literals, resolved calls, matmul, reduceSum, and shape checks. Matmul records separately rounded products and balanced adjacent reduction. ReduceSum records balanced reduction over row-major logical elements. Later lowering must implement the accepted odd-tail, empty, rounding, and cleanup behavior.

Value IDs occupy a flat arena. Functions own their values and ordered statement roots. Arena insertion order is not an execution schedule: lowering must follow statement order and ordered operands/arguments to preserve once-only, left-to-right evaluation. Pipelines become ordinary resolved calls with the left-hand expression inserted once as the first argument.

Runtime guard metadata is not an executed runtime check yet. The next stage must lower shape checks and dynamic allocation-size checks before the affected allocation/computation and preserve immutable storage ownership. Publishing a typed module does not establish that the native runtime is complete.

## Failure and resource limits

Syntax errors pass through unchanged and prevent semantic analysis. Semantic errors retain source spans and prevent publication of the entire typed module; diagnostics may accumulate across independent functions. There is no invalid/dynamic placeholder type in successful IR.

The checker bounds expression/function analysis depth, typed values, literal elements, numeric spelling length, shape-requirement work, and diagnostics. Defaults are depth 128, 300,000 values, 200,000 tensor elements, 4,096 bytes per numeric literal, 100,000 shape-requirement steps, and 32 diagnostics. These are configurable implementation limits, not new language type/rank rules. A syntax-valid chain can currently exceed the semantic recursion budget and receive a diagnostic; eliminating that limit through an iterative checker is later performance work.

## Verification and native lowering

The 13-test CTest suite includes syntax and semantic tests with AddressSanitizer/UndefinedBehaviorSanitizer, CLI success/error checks, and the existing native MLIR proof. Semantic cases check exact literal rounding at ties, subnormals and overflow, forward/recursive calls, pipelines without duplicated calls, immutable bindings, tensor construction, known/dynamic shapes, call-result extent substitution, wrapper requirements, overflow-safe shape metadata, and bounded failures. Tests validate symbol/value ownership and refuse partial IR.

[Native MLIR/LLVM lowering](native-codegen.md) now compiles this checked IR into an executable, including runtime guards and ownership cleanup. The native CPU conformance sweep passes; the handwritten proof remains independent reference evidence.

## Task and TaskError checking

The initial local task subset adds explicit task-operation and parallel-block IR. Value metadata distinguishes a group-owned handle, a completed value, and a fallible TaskError result without replacing concrete scalar/tensor payloads with Any. Aliases retain the original task identity. A successful group must observe all its child identities; handles cannot be returned, passed as ordinary payloads, or escape through a group result. Nested block bindings remain lexical.

Await produces the child's concrete success type with a TaskError channel, flattening that same error family for an already fallible child. Try requires a fallible value and a lexical parallel or explicitly fallible function boundary. Discarded expected-error results and implicit use of a result as a success value are diagnosed. Named TaskError functions require an explicit error-channel annotation in this initial subset. Catch handlers now receive a TaskError value inferred from the operand's error channel. TaskError, FaultKind, and the bounded diagnostic String fields have distinct value categories; none can be used as a float32 or hidden in a numeric tensor. Exhaustive matches check variant families, duplicate/unreachable cases, explicit fallbacks, selected fields, binding aliases, and branch success types. Pattern fields and handler parameters remain local and immutable.

A nonfallible handler removes the operand's error channel. A handler returning a fallible result, using try at its own boundary, or re-propagating with fail leaves one flattened TaskError channel. Try/fail inside that handler target the handler or an inner parallel group, not the enclosing function/group. This initial fail implementation requires a concrete numeric/tensor success context; bare-error construction and general bottom-type inference are not implemented.

Handler and match branches get independent obligation states. A newly created expected result cannot be silently discarded on its creating path. Observation/handling of an incoming task/result counts after a match only when every branch performs it; a handler alone cannot discharge an incoming obligation on the success path where it never runs. This is conservative structural checking, not general path-sensitive analysis of branches that terminate early.

Fallback tensor dimensions merge with success dimensions; a different dynamic extent is not silently treated as the success path's known shape. Shape constraints within conditional bodies remain runtime checks in those bodies and are not lifted into unconditional function preconditions. Refining a fallible tensor preserves its error without inspecting an absent descriptor.

Arbitrary application error families, unions, general variants, and broader error inference remain future implementation work. Built-in message/reason fields can be bound and aliased as String values; general string literals, operations, and String function signatures remain outside this numerical subset.

Checks cover inline and repeated awaits, observation through aliases, use outside groups, discarded results, handle escapes, and missing propagation. [Native source task tests](native-tasks.md#compiled-source-task-subset) now exercise these checked operations through actual continuation lowering, tensor kernels, and runtime settlement. The complete native suite contains 25 CTest tests.

The [shared CPU/GPU helper path](apple-gpu.md#shared-cpugpu-helpers) now consumes the same checked tensor/scalar function bodies for both targets. GPU capability admission is separate from type correctness: unsupported GPU operations are rejected even when the function is valid on CPU. Supervisor task placement and residency remain target-specific checks.

## Shared placement checking

The optional [placement-aware check](placement-ir.md) extends this same typed module with `DeviceSelection`, owned `Transfer` tasks, checked spawn targets and exact tensor residency. It reuses group/error obligations and shape checking; it does not create a separate semantic type system for Metal. The normal CPU checker retains its existing policy, and CPU backends reject placement-bearing IR. Metal compilation must pass the shared check before publishing its artifacts, while device capability admission and the current syntax-driven executable emitter remain target-specific.

## Current numeric source scope

The initial float32-only subset above has expanded to checked int32/uint32/int64/uint64 CPU scalars and tensors, plus [strict float64 CPU scalars](numbers.md#cpu-float64-scalar-source-and-worker-integration). Whole-number literals default to int64; decimal/exponent literals default to float64. Literal contexts select concrete types without introducing implicit conversions between already typed values. The `int`, `uint` and `float` aliases now have executable scalar implementations. [Synchronous float64 tensor literals and elementwise operations](numbers.md#cpu-float64-tensor-literals-and-elementwise-arithmetic) are now qualified. [Float64 tensor worker captures/results](numbers.md#float64-tensor-captures-and-results-in-cpu-tasks) are also qualified. [CPU float64 reductions](numbers.md#cpu-float64-reducesum) are also qualified. [CPU float64 matrix multiplication](numbers.md#cpu-float64-matmul) is also qualified. [Explicit CPU scalar conversions](numbers.md#explicit-cpu-scalar-numeric-conversions) now support all 36 pairs among the six implemented scalar types. [Explicit CPU tensor conversions](tensors.md#explicit-cpu-element-conversions) now preserve shape and use the same checked per-element rules. Implicit conversions remain disabled; GPU conversions and GPU float64 arithmetic remain gated. See the numeric qualification record for the current operations and worker support.

## CPU Boolean values and comparisons

[Bool values and scalar comparisons](booleans.md) now use a distinct builtin identity, BoolLiteral and ScalarCompare typed operations. Numeric comparison operands preserve their concrete types; Boolean result context does not select numeric precision. Completed CPU scalars of the same numeric type support six comparisons; Bool supports equality/inequality. Tensor and unobserved task/error operands remain rejected. Boolean captures/results use distinct result layouts and the existing continuation planner; GPU Bool execution remains gated.

## CPU conditional evaluation

[Conditionals and logical operators](control-flow.md) now use ConditionalOperation with a completed Bool condition and two typed statement/result branches. Branch scopes and task-obligation state are copied independently; only observations common to all paths discharge incoming obligations. Joins retain concrete numeric identity, category/rank/residency and common shape facts. Synchronous CPU lowering uses native MLIR branches; continuation lowering uses explicit Boolean dispatch and existing result copies. Bool fail/rethrow contexts are also supported.

[Native tensor inference](tensors.md#native-tensor-element-and-shape-inference) now determines element identity and literal shape without an expected tensor type. It checks each leaf once in source order, requires homogeneous completed numeric scalars, and reuses the existing typed tensor literal operation. Empty data still requires element context.

[Immutable native numeric arrays](arrays.md) now retain a distinct source category across annotations, typed literals, calls, branch joins and task continuations. Runtime storage reuses checked rank-one buffers, while source conformance and IR capability checks reject implicit tensor/array interchange and unsupported operations.

[Sequential array maps](array-map.md) now lower to a typed `ArrayMap` with a checked input array and resolved callback function. Output length facts are preserved, output element identity follows the callback, and transitive resumable callbacks are rejected before lowering. Inline anonymous numeric callbacks lift only referenced lexical CPU values into private parameters; `ArrayMap` records those capture operands explicitly. Worker kernels import synchronous callback/helper definitions with remapped function, value, symbol and capture identities.

[Runtime array-to-tensor construction](arrays.md#explicit-tensor-construction-from-arrays) uses an explicit `ArrayToTensor` typed operation. It preserves numeric identity and rank-one extent facts while changing the source category. CPU lowering checks storage size and copies elements in order; continuation kernels and imported callback helpers remap the input operand without treating arrays as implicit tensors.

[Runtime array reshaping](reshape.md) adds optional dimension operands to `ArrayToTensor`. The output rank is fixed by the source list; each operand is a completed integer scalar. Known extent/count contradictions are rejected, expected fixed extents are refined into pre-allocation guards, and dynamic dimensions pass through continuation kernels and imported callbacks. CPU lowering validates byte counts and row-major suffix strides, including zero-element shapes, before an ordered flat-to-ranked copy.

[Checked CPU tensor indexing](tensor-indexing.md) now uses `TensorIndex` with an ordered integer operand per dimension, preserving scalar element identity and rejecting rank, index-type and residency mismatches. Array reads retain `ArrayIndex`; both share checked CPU extraction and Bounds recovery. Continuation kernels/imported callback graphs remap all indices, including resumable operand expressions.

[CPU collection queries](collection-queries.md) use `CollectionQuery` with an explicit Length/Shape kind and input. Length returns int64; Shape returns a managed int64 array whose private length equals the input rank. Capability checks preserve categories and result facts; continuation kernels and imported callback graphs remap the input.

[CPU tensor reshaping](tensor-reshape.md) uses `TensorReshape` with an explicit source tensor and ordered dimension operands. It shares dimension/stride validation with array construction, checks known element-count contradictions and leaves unknown count equality to runtime guards. Capability validation preserves element identity and output rank; continuations remap all operands through imported helpers and callbacks.

[CPU tensor-to-array construction](tensor-array.md) uses a distinct `TensorToArray` operation with one tensor operand. The result is an array of the same numeric type with a length equal to the product of source extents; known counts and rank-one extent identities survive analysis. CPU lowering reuses checked logical copying from reshape, and continuation import/clone paths remap the input. Unsupported categories, residency and element changes are rejected before lowering.

[Local CPU modules](modules.md) now resolve named exports/import aliases before function analysis. Function lookup is module-local, including imported helpers, named map callbacks and nested anonymous callback bodies. Dependency functions retain unique identities in shared typed IR and cannot become the root entry point accidentally. Composite byte spans map diagnostics and generated locations back to original source files.
