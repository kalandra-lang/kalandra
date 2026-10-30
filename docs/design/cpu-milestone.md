# First native CPU compiler milestone

Accepted implementation sequence — 2026-10-15. The first sequential float32 CPU milestone is implemented and its conformance sweep passes on macOS ARM64. The [application walkthrough](compute-example.md) is accepted as the integration direction. This milestone implements its sequential CPU calculation, using the fresh language rules rather than extending the prototype's JavaScript emitter.

The deliverable is a compiler path from a `.k` source file to a standalone native executable. The executable computes float32 163 from the projection fixture without Bun, Node, a JavaScript engine, a GPU, or a service. A separate runtime-input harness verifies that real computation remains after constant folding is disabled or inapplicable.

## Acceptance program

The canonical source is [cpu-projection.k](fixtures/cpu-projection.k). It constructs the two example matrices and calls reference, which calls project and summarize. Main returns float32 163. Functions returning tensors must work across a function boundary; this cannot depend on inlining every call.

For this milestone, the compiler-generated executable wrapper accepts `fn main() -> float32`, writes the successful scalar result as one decimal line to stdout, and exits zero. It reports compilation/runtime faults to stderr and exits nonzero. This is a development harness convention, not the final language entry-point or printing API. Arbitrary float results must round-trip; the fixture's exact stdout is `163` followed by a newline.

The initial supported native target is the development machine's macOS ARM64. Other targets require separate build/runtime validation. This limits the first proof, not the language's long-term portability.

## Deliberately small supported subset

| Area         | Required in this milestone                                                                           |
| ------------ | ---------------------------------------------------------------------------------------------------- |
| Source       | One UTF-8 file, indentation blocks, multiline parenthesized forms, source spans                      |
| Bindings     | Immutable local bindings, optional annotations, lexical lookup, duplicate-name diagnostics           |
| Functions    | Named definitions, typed parameters, declared returns, parenthesized calls, final-expression return  |
| Scalars      | float32 values and literal contextual typing; index/extent arithmetic checked internally             |
| Tensors      | Dense ranked float32 tensors; static extents and dynamic `?` parameter extents                       |
| Construction | Rectangular numeric nested literals with known rank and shape                                        |
| Operations   | Rank-two matmul and reduceSum under the accepted fixed-tree numerical contract                       |
| Memory       | Safe owned tensor results across calls, immutable inputs, checked allocation sizes, complete cleanup |
| Diagnostics  | Syntax/name/type/rank/shape errors and explicit unsupported-feature errors                           |

This is a compiler subset, not a revision of the accepted language. No universal Number or Any is introduced to fill gaps. Other accepted numeric types, tensor elementwise operations, records, arrays as general values, dynamic-data constructors, variants, expected-error channels, closures, modules, and resource APIs follow in separate increments. Nested arrays in the fixture are contextual tensor-constructor input, not proof of general array support.

Parallel, spawn, await, on, transfer, and device selectors were deliberately outside this first sequential executable. The subsequent [source task increment](native-tasks.md#compiled-source-task-subset) now implements local parallel/spawn/await and TaskError propagation. Reject them clearly instead of accepting syntax with sequential stub behavior. Numeric spellings outside the implemented subset also receive an unsupported-type diagnostic rather than being silently represented as float32.

No arbitrary rank restriction is added to the language: the first constructor/reduction implementation may explicitly support rank-two tensors only, with matmul's existing rank-two requirement. Supporting a type in the design does not obligate the first milestone to implement every operation on it.

## Compiler structure

Implement the fresh compiler in **C++23**, building with CMake/Ninja and testing with CTest. Use a native indentation-aware lexer and a small recursive-descent parser, adding expression precedence handling as the supported subset grows. LLVM/MLIR are direct C++ dependencies. The native compiler build and tests do not require Bun or Node. The released prototype and Tiramisu website keep their existing tooling; their JavaScript grammar and semantic analyzer are not the definition of the fresh language. See [native development](../../native/README.md).

The intended pipeline is:

```text
source
  → tokens and indentation-aware syntax tree
  → resolved, typed IR with tensor shapes and source locations
  → MLIR tensor operations and explicit numerical/checking semantics
  → bufferization and ownership lowering
  → scalar/control-flow/memory lowering
  → LLVM IR
  → object file plus a minimal native harness
  → standalone executable
```

The typed frontend IR preserves matmul and reduceSum as operations. Their shape and numerical contracts must survive until an appropriate lowering is selected. Do not expand them into JavaScript loops or discard their identity at parsing time.

Build MLIR through its native C++ API from resolved, typed frontend IR. Verify each lowering stage; source identifiers must resolve to compiler-owned symbols rather than being spliced into IR text. Textual MLIR remains useful for debug output and backend reference fixtures, not as a TypeScript-to-native bridge. A custom dialect is needed only where standard operations cannot retain the required semantics safely. The toolchain proof now includes a C++23 executable that parses and verifies its MLIR fixture through the linked API; native pass-manager integration follows with the compiler backend.

MLIR converts tensor semantics to memory references through bufferization. One-Shot Bufferize does not insert all deallocation itself; ownership/deallocation is a separate part of the pipeline. Pin and test both. [MLIR bufferization](https://mlir.llvm.org/docs/Bufferization/).

MLIR's LLVM dialect and translation provide the route to LLVM IR; the selected pass pipeline must eliminate or legally lower every remaining operation. Verification failure must stop compilation. [MLIR LLVM IR target](https://mlir.llvm.org/docs/TargetLLVMIR/).

IREE remains a candidate for the later device runtime. It is not an additional mandatory runtime for this first standalone CPU proof. Its evaluation must use the same numerical and lifetime checks; adopting it cannot silently change those contracts.

## Numerical correctness before optimization

This section defines the fixed-tree baseline retained by the CPU implementation
and explicit software arithmetic. Hardware GPU float32 matmul now additionally
supports the default [optimized full-precision policy](tensors.md#precision-reductions-and-optimization);
`--matmul-policy fixed-tree` retains the baseline below. Optimized tensor
accumulation does not relax scalar arithmetic, ReduceSum or checked integers.

Do not lower strict matmul as an unspecified vendor reduction or enable generic fast-math flags. For each output element, round each product to float32, then reduce products with the accepted balanced tree in ascending contraction-index order. ReduceSum uses row-major logical order with the same tree; carry an unpaired value to the next level and use zero only for an empty reduction.

The first implementation can use a conservative explicit loop/tree lowering. It need not have competitive matrix performance yet. A standard linalg representation is usable only if its lowering preserves the required order and rounding; an unchecked reassociating matmul pipeline is not sufficient evidence.

No FMA contraction, reassociation, reduced-precision multiplication, or flush-to-zero substitution is permitted. Native arithmetic lowering must preserve the accepted floating-point behavior; merely omitting fast-math flags does not prove the whole pipeline is correct. Check operation semantics and backend options, then test rounding-sensitive inputs. [MLIR arithmetic dialect](https://mlir.llvm.org/docs/Dialects/ArithOps/).

## Memory safety boundary

Represent tensors with validated dimensions, element type, and managed backing storage. Check dimension products, allocation byte counts, indexing, and allocation failure before memory access. Internal machine-sized indices are an implementation detail, not a new source numeric type or an unchecked cast from dimensions.

For the first dense-tensor subset, choose and document one explicit internal ownership convention for arguments, returned buffers, temporary reduction storage, and views before generating allocations. A destination-passing function ABI is acceptable: the caller owns output storage while the source function still returns an immutable logical tensor. It must handle runtime extents and failure cleanup safely.

Do not return pointers into callee stack storage, free aliased input buffers, or treat bufferization alone as memory management. A process-lifetime allocation arena that leaks every call does not meet this milestone. A minimal native runtime may contain carefully bounded low-level code, with generated checks and sanitizer evidence; its safety is something to verify, not something the choice of LLVM guarantees.

This milestone does not select the full language's cycle collector or prove closure/resource/device lifetimes. Its ownership mechanism only needs to cover the declared subset without preventing later managed values. Runtime faults stop the sequential execution unit with diagnostics; expected-error recovery and supervised TaskError conversion are later features.

## Implementation gates

Complete these in order. A failed gate narrows or repairs the implementation before proceeding.

1. **Pinned toolchain proof.** Choose one LLVM/MLIR revision and compatible native build tools. Verify a tiny hand-written tensor program through bufferization, ownership cleanup, LLVM lowering, linking, and execution on macOS ARM64. Save exact versions and reproducible commands. Confirm the required strict reduction lowering is feasible before depending on a broad stock pipeline.
2. **Fresh syntax and diagnostics.** Parse the canonical source into an independent AST with source spans. Test indentation, multiline constructors/calls, contextual tensor syntax, malformed input, and unsupported constructs. Never use the prototype parser's successful parse as conformance evidence.
3. **Resolution and typed IR.** Resolve function/local names, context-type literal elements as float32, infer literal shapes, check calls/returns, and represent static/dynamic extents. Preserve both source locations and numerical policy on tensor operations.
4. **Native tensor execution.** Lower and execute the canonical calculation. Retain function-boundary correctness with inlining disabled. Run runtime-supplied tensor contents through the same compiled project/reference functions via a private test ABI, without inventing a public FFI yet.
5. **Failure and lifetime evidence.** Exercise runtime extent validation, empty dimensions, overflow-safe allocation sizing, allocation failure injection, repeated calls, and immutable input reuse under native sanitizers. Inspect both diagnostics and cleanup.
6. **Reproducible compiler command.** Provide a development build command accepting source and output paths, runnable from a clean checkout. Store intermediate IR on request and report missing toolchain components clearly. This is not an npm release or replacement for the prototype CLI yet.

**Gate 1 completed — 2026-10-15.** Existing Homebrew LLVM/MLIR/Clang 22.1.8 was available outside PATH. The [native toolchain proof](../../native/toolchain-proof/README.md) now runs tensor bufferization, ownership/deallocation, loop lowering, LLVM translation, linking, and native execution. Debug, optimized, and sanitizer builds pass 49 float32 cases each, 10,000 allocation/release checks each, and injected allocation failure. The checked-in toolchain lock validates exact tool versions; no additional install was needed. CMake/CTest now orchestrate this proof, a C++23 reference replaces the Bun reference, and a native C++ MLIR API check accepts valid input and rejects a type error. This is hand-written fixed-shape MLIR evidence, not yet source-to-native compiler support. Gate 2 is now implemented as the [handwritten C++23 syntax frontend](frontend.md): the projection source parses with source spans, diagnostics, recovery, and sanitizer-tested limits. Gate 3 is now implemented as [resolution and typed IR](semantics.md), checking the canonical source into four functions and 26 typed values with shape guards and strict numerical metadata. The 13-test suite passes, including instrumented syntax/semantic libraries. [Native source-to-executable lowering](native-codegen.md) now implements gate 4 and the initial gate 5/6 checks: the canonical program prints 163, runtime-supplied matrices execute through the source-compiled functions, and debug/optimized/sanitized tests cover guards and allocation cleanup. The CPU conformance sweep now passes all 16 CTest suites, including 1,029 runtime shape/content combinations in each debug/optimized/sanitized source suite, shared/nested tensor lifetimes, allocation failure cleanup, and call-argument fault order. A fresh source-only build with a restricted PATH also passes on this host using the already installed pinned toolchain. See the [ABI, prerequisites, and conformance limits](native-codegen.md).

## Required verification

| Evidence                                                         | What it establishes                                                                         |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| Canonical executable prints 163                                  | Complete source-to-native path for the supported subset                                     |
| Runtime contents differ across invocations                       | Computation is not only a precomputed fixture constant                                      |
| Returned tensors survive uninlined callees                       | Function ABI and storage lifetimes are valid                                                |
| Static inner-dimension mismatch is rejected                      | Compile-time shape checking                                                                 |
| Dynamic inner-dimension mismatch faults before compute           | Runtime shape checking, tested through a private harness                                    |
| Known ragged literal, wrong dtype/rank/return, unknown name      | Useful frontend diagnostics with locations                                                  |
| Empty contraction/output/reduction cases                         | Defined zero-sized behavior and no invalid accesses                                         |
| Rounding-sensitive sums/products, odd tree lengths               | Fixed reduction order and product rounding                                                  |
| Negative zero, subnormals, infinities, NaNs                      | Accepted IEEE behavior; no promise of NaN-payload identity                                  |
| Repeated calls and injected allocation failure under sanitizers  | No observed leak, use-after-free, invalid access, or skipped cleanup in the exercised paths |
| New executable runs with JS runtimes absent from its environment | Neither compiler tooling nor the executable requires Bun/Node                               |

For dynamic mismatch tests, the private ABI validates incoming descriptors rather than trusting arbitrary pointers/extents. Malformed native pointers themselves are outside a public safe-FFI claim. The harness supplies valid owned storage with incompatible logical shapes to test language validation.

Use a small independent scalar reference for deterministic results and failures; do not compare two paths sharing the same lowering bug. IR snapshots supplement behavior tests but cannot replace native execution. Sanitizers provide evidence for exercised paths, not a proof of the entire future language's memory safety.

## Done and next

Completed for the declared sequential float32 subset: a fresh source copy builds the supported program into a native executable; runtime-input, numerical, failure/lifetime, and sanitizer suites pass; and the private ABI and toolchain prerequisites are documented. Toolchain installation on a blank host and distribution packaging are not claimed. A green prototype JavaScript test suite does not substitute for native evidence.

The [native local-task contract](native-tasks.md) specifies the next implementation sequence. Checked invocation contexts and managed tensor ownership now replace process-global fault cleanup on source-generated paths, with native tests covering the existing calculations and new fault-isolation/ownership cases. The single-worker core now adds synthetic-entry tests for resumable waits, bounded admission, cached outcomes, and group settlement, bringing the suite to 22 tests. The first source task subset now implements these checks and native continuation lowering, with 25 CTest tests covering the numerical/runtime baseline and source task programs. Next complete native TaskError handling before multiple-worker execution. GPU and remote backends follow their own capability, transfer, residency, and deployment gates. Optimize only after the strict baseline has executable evidence.
