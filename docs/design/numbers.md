# Kalandra numeric types and arithmetic proposal

**Current execution policy:** [Hardware arithmetic and explicit software fallback](hardware-arithmetic.md) is the default policy as of 2026-10-24. CPU float32/float64 and Metal float32 use hardware arithmetic. Metal float64 arithmetic requires explicit fallback permission. Exact-bit software proofs below describe the opt-in software implementation, not the default hardware path.

Accepted numeric design — 2026-10-14. The project owner approved the default widths, literal typing, conversions, and arithmetic rules below. They describe the fresh implementation, not behavior of the released Kalandra 0.13 compiler. Error propagation is accepted separately; tensor/reduction extensions are accepted in the tensor design.

Use convenient defaults for everyday code and explicit widths for memory layout, native interfaces, and tensors. Arithmetic should have predictable types, diagnose mistakes, and preserve safety in optimized builds. CPU, GPU, and remote execution must not silently change a declared type or precision.

## Accepted defaults

| Name    | Meaning                      | Explicit equivalent |
| ------- | ---------------------------- | ------------------- |
| `int`   | Signed 64-bit integer        | `int64`             |
| `uint`  | Unsigned 64-bit integer      | `uint64`            |
| `float` | IEEE binary64 floating point | `float64`           |

These aliases have the same meaning on every target. They are not pointer-sized types. Native pointer sizes and array indexing representations are separate ABI/runtime decisions, not reasons to vary public numeric types.

Support `int8`, `int16`, `int32`, `int64`, their unsigned equivalents, and `float32`/`float64` in the first numeric core. Evaluate `int128`/`uint128`, `float16`, and `bfloat16` in the tensor/extended-number proposal. A format can be represented or serialized even when a device cannot efficiently perform arithmetic on it; representation support and arithmetic capability must be specified separately.

The 64-bit defaults favor range and stable cross-target meaning. Compute-focused programs choose narrower types deliberately. A later change to the alias defaults would be a language-level breaking decision, not a backend optimization.

## Literals and inference

```kalandra
count = 3
ratio = 0.5
capacity: uint = 32
sample: float32 = 0.5
```

Unconstrained whole-number literals infer `int`; decimal or floating-exponent literals infer `float`. An expected numeric type may type a literal directly, provided its value is valid for that type. This is distinct from converting an already typed variable.

```kalandra
fn double(value: int32) -> int32:
  value * 2

answer = double(21)
```

The parameter context types `21` as int32. The known int32 operand types the literal `2` as int32; the result is int32. Context does not search for the smallest representation or change a variable's established type.

Integer literals are parsed with enough compile-time precision to perform range checks, including the signed minimum written with a leading minus. Their runtime types remain fixed-width. Fractional literals cannot initialize integer types. Float literals round to the declared format using round-to-nearest, ties-to-even; overflow of a finite literal is diagnosed rather than silently producing infinity.

Invalid:

```kalandra
negative: uint = -1
tooLarge: uint8 = 256
fractional: int = 1.5
```

Literal format details such as hexadecimal notation, digit separators, and suffixes are a later grammar decision. Explicit annotations are sufficient for these examples.

## No silent mixing of typed values

Ordinary numeric binary operations require operands of the same concrete type. Aliases count as their explicit equivalents. A literal can take the other operand's numeric type; an already typed variable cannot silently widen, narrow, or switch family.

```kalandra
count: int = 3
ratio: float = 0.5
mixed = count + ratio
```

Reject `mixed` and suggest an explicit conversion. Likewise, adding an int32 variable to an int64 variable requires an explicit conversion. There are no C-style signed/unsigned promotion rules.

A homogeneous array has one element type. With no expected array type, integer and floating literals do not silently establish a mixed/promotion rule. Write `values: [float] = [1, 2.5]`; the array context types both literals as float. An unannotated `[1, 2.5]` needs an annotation under this initial rule.

Generics and numeric constraints are separate design work. A monomorphic function with an `int` parameter does not automatically specialize into int32 or float variants merely because a caller supplies those types.

## Explicit conversion

Use a numeric type as a conversion function:

```kalandra
count = 3
ratio = float(count) / 2.0
```

`ratio` is float 1.5. Conversion has the same ordinary call spelling as other operations and requires no decorator or special cast operator.

| Conversion         | Behavior                                                                                   |
| ------------------ | ------------------------------------------------------------------------------------------ |
| Integer to integer | Preserve the value; fail if it is outside the destination range                            |
| Integer to float   | Round to the destination format; precision loss is explicitly authorized by the conversion |
| Float32 to float64 | Preserve the represented value exactly                                                     |
| Float64 to float32 | Round to nearest, ties-to-even; fail if a finite source overflows to infinity              |
| Float to integer   | Truncate toward zero; fail for NaN, infinity, or an out-of-range truncated value           |

Floating-width conversions preserve infinity and NaN as nonfinite values; preservation of a NaN payload is not part of this initial contract. Lossless conversions may be optimized away. Known invalid conversions are compile-time errors; runtime-invalid checked conversions produce an arithmetic failure. The accepted [errors design](errors.md) classifies it as an ArithmeticFault that stops the execution unit. Explicit checked conversion helpers provide recoverable error results.

Passing a correctly typed value needs no conversion. Implicit conversion from strings, booleans, or arbitrary foreign objects is not part of the numeric core.

## Integer arithmetic

Addition, subtraction, multiplication, and unary negation preserve operand type. Overflow is checked by default on every build mode. A known overflow is diagnosed during compilation; a dynamic overflow produces an arithmetic failure. The compiler removes checks only when it proves they unnecessary.

```kalandra
largest: uint8 = 255
overflow = largest + 1
```

This does not produce zero or undefined behavior. It is a known invalid operation and is rejected.

Wrapping arithmetic may be provided through explicitly named library operations, such as `wrappingAdd`, for algorithms that require modular arithmetic. It must not become the release-build meaning of ordinary `+`. Saturating arithmetic is another explicit library operation, not the default.

Integer division with `/` produces the same integer type and truncates toward zero:

```kalandra
whole = 5 / 2
fraction = 5.0 / 2.0
```

`whole` is int 2; `fraction` is float 2.5. Integer division by zero and signed minimum divided by -1 fail. A caller wanting fractional division converts explicitly or uses floating operands.

`%` is integer remainder, consistent with truncating division: `-5 % 2` is -1 and `5 % -2` is 1. The quotient/remainder identity holds where the division is defined. Remainder by zero fails; signed minimum remainder -1 is mathematically zero and must be implemented without invoking an overflowing division. A nonnegative modulo helper can be provided separately.

Division has one documented rule per operand family, with no hidden result-type promotion.

## Floating-point arithmetic

Float32 operations produce float32; float64 operations produce float64. Finite results round to the declared format. Values are not silently accumulated or evaluated with a different source-visible precision because another target permits it.

The baseline has strict floating-point semantics, including signed zero, infinities, and NaN. Floating-point division by zero follows IEEE behavior rather than the integer failure rule: positive finite divided by positive zero yields positive infinity; zero divided by zero yields NaN. Finite arithmetic overflow can produce infinity; the explicit literal/conversion checks above are separate entry-validation rules.

NaN is unequal to every value, including itself. Ordered comparisons involving NaN are false. Positive and negative zero compare equal. Finiteness checks belong to ordinary numeric library functions. Serialization/contracts must state their support for nonfinite values instead of silently replacing them with zero.

Do not reassociate floating-point expressions or contract multiply/add into a fused operation under the baseline if that changes rounding. An explicitly named fused operation can request that behavior. Fast-math requires a separate explicit policy stating which guarantees are relaxed; it is not an automatic consequence of a production build or GPU target.

This baseline requires evaluating hardware support for operations, subnormals, rounding, and precision. If a target cannot implement the promised behavior, diagnose the unsupported operation/target or offer an explicitly selected alternative policy. Do not silently substitute float32 for float64 or flush subnormal values without declaring that policy.

Reduction order is an execution-level numerical decision. A sequential reduction and a parallel tree reduction may yield different rounded results. The future reduction API must expose its reproducibility/tolerance contract; parallel scheduling cannot arbitrarily redefine a promised deterministic result.

## CPU, GPU, contracts, and native interfaces

Public signatures and tensor element types have fixed widths. Contract generation preserves signedness, width, and float format. A JavaScript client cannot losslessly represent all int64/uint64 values as JS Number; its generated representation must use an appropriate lossless type such as BigInt or an explicitly defined external encoding.

The compiler may use wider intermediates internally only when results, failures, and rounding remain equivalent to the specified operation. It cannot replace checked overflow with a different arithmetic model.

Tensor precision is explicit at the tensor boundary. CPU/GPU capability checks occur before dispatch where possible. The initial 64-bit scalar defaults are convenience choices, not a claim that every accelerator efficiently supports 64-bit arithmetic.

C ABI types are mapped using the target ABI. Do not equate C `int` or `long` with Kalandra's `int` alias merely because their names look similar. Native conversion and layout need explicit binding metadata.

## Examples to turn into conformance checks

1. Defaults are int64, uint64, and float64 on every supported target.
2. Literal typing works through annotations, parameters, and binary operations.
3. Integer endpoints, signed minimum parsing, and out-of-range literals are checked correctly.
4. Mixed typed operations require explicit conversions.
5. Conversion rounding, truncation, boundaries, NaN, and infinity follow the rules above.
6. Integer overflow/division checks remain effective in optimized builds.
7. Negative quotient/remainder cases and the signed-minimum edge cases are defined safely.
8. Float32 and float64 retain declared rounding behavior without unrequested reassociation or fusion.
9. Unsupported device precision/semantics are diagnosed.
10. Generated contracts preserve values that exceed JavaScript's exact Number range.

## Review and next step

The alias widths, checked arithmetic, division/remainder rules, literal typing, and conversion spelling are accepted. The [memory programming model](memory.md) is automatic and allocation strategy belongs to the compiler.

The [errors and effects design](errors.md) now accepts expected errors, inferred effects, and the execution-fault treatment for invalid ordinary checked operations. [Resources and task lifetimes](execution.md) are accepted. [Variants and error payloads](variants.md) are accepted. [Tensor types and shapes](tensors.md), including shape failures and reduction policies, are accepted. Review [placement and transfers](placement.md) next.

## Shared numeric descriptions in the native implementation

The native compiler now carries an explicit concrete numeric identity on scalar and tensor types. [NumericType.h](../../native/include/kalandra/NumericType.h) describes the ten accepted core types: signed and unsigned 8/16/32/64-bit integers, float32 and float64. The catalogue records canonical spelling, numeric family and fixed bit/byte width. `int`, `uint` and `float` resolve to the accepted 64-bit equivalents independently of host pointer size. These are internal compiler identities, not a serialized protocol or target ABI declaration.

**This is type infrastructure, not arithmetic support for all ten types.** The initial implementation restricted CPU/Apple GPU arithmetic to float32. Checked CPU int32 and uint32 scalars/tensors are now qualified below; Apple GPU arithmetic remains float32-only. Int32 CPU scalar tasks and synchronous tensor literals/elementwise operations are qualified below. Recognizing a type's description does not enable its literals, conversions, operators, tensor kernels or runtime interfaces. The deferred int128/uint128, float16 and bfloat16 formats are not added to the core catalogue by this step.

Numeric identity now survives signatures, scalar/tensor operations, shape checks, calls, tasks, expected errors, branches and continuation slots. Type conformance and arithmetic/branch joins compare the concrete identity, rather than treating all numbers as the same type. IR inspection prints the concrete name; float32 program dumps remain unchanged. CPU lowering rejects unsupported numeric identities before constructing float32 MLIR or LLVM task layouts, including unsupported identities in a declared result, symbol or expression. The Metal supervisor and GPU closure admission retain explicit type capability checks.

Storage size and arithmetic capability are separate concerns. Checked byte counts use the concrete element width and an explicit maximum, rejecting overflow and invalid identities while supporting valid empty allocations. Semantic tensor allocation checks and the Metal storage bridge use these descriptions. The bridge's existing private float32, uint32 and uint8 formats map explicitly to numeric identities; uint32 storage does not imply integer shader support; CPU uint32 scalar and synchronous tensor arithmetic are qualified below. Existing float32 memref descriptors and scalar result layouts retain their ABI and numerical behavior.

[Numeric type tests](../../native/tests/NumericTypeTests.cpp) cover aliases, unique identities, storage limits and overflow, empty sizes, invalid/deferred formats, typed task-plan metadata, private storage mapping and rejection of unsupported numeric IR at every CPU entry surface. The current capability checks admit CPU uint32 scalars and synchronous tensor arithmetic while retaining unsigned reduction/matmul and GPU gates. They run in debug and ASan/UBSan variants alongside the complete CPU/Metal regression suite. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/numeric-descriptions-2026-10-16.json) records the regression and numerical artifact comparisons.

The element-aware CPU runtime storage and adapters described below now cover retention, copying and scalar task layouts. Next, qualify an additional concrete source arithmetic type end to end. Integer overflow/conversion rules, floating-format rounding and strict GPU arithmetic still need per-type implementations and conformance evidence. Shared descriptions allow that work to reuse shape, ownership, scheduler and placement logic; they do not replace those arithmetic proofs. No distributed execution or protocol work is included.

## Element-aware CPU runtime storage and adapters

The native runtime now has `CpuTensor<Element, Rank>` descriptors and numeric scalar task layouts for all ten core types. [NumericStorage.h](../../native/runtime/NumericStorage.h) maps concrete language identities to fixed-width C++ storage and the host ABI's alignment. [Tensor.h](../../native/runtime/Tensor.h) defines ranked memref descriptors, checked retention and independent owned copies. This is storage/ABI infrastructure; the separate checked CPU int32 scalar increment below enables arithmetic only where it has been qualified.

The descriptor contains only the fields required by the MLIR memref ABI: allocated/aligned pointers, element offset, and ranked size/stride arrays. Its element type is carried by the C++ template and the separate adapter view. The float32 alias retains the existing descriptor layout, including the special rank-zero representation. Raw views default to an invalid element tag; typed descriptors supply the tag automatically. Views are trusted native adapters, not language-level casts or a dynamic foreign-object interface.

`retainCpuTensor` validates the numeric identity, dimensions, dense row-major strides, alignment, element-width-aware offset and complete allocation bounds against the source Invocation. It snapshots dimensions and retains an immutable allocation lease. The retained bytes remain readable after source metadata and invocation owners disappear. Empty tensors validate their type and dimensions but do not dereference or retain their descriptor addresses. A failed validation publishes no retained value.

Typed CPU copies require an exact element match before allocation. They compute canonical strides with overflow checks, copy bytes into independently owned invocation storage, and preserve the represented bits without numeric conversion. This includes floating NaN payloads and integer sign bits. Allocation failure and invalid rank/type/size leave no output allocation. Invalid or unsupported element types are never guessed from their byte width.

Scalar task layouts have distinct type identities, correct host alignment and fixed-width bit-copy functions. The float32 bridge uses the same canonical layout as the generic scalar factory and retains its existing private layout identity. The scheduler's exact layout check rejects reading a uint32 result as float32 even though both occupy four bytes. Retaining, completing, observing and cleaning up values still use the existing scheduler and ownership mechanisms.

The Metal bridge uses the common CPU validator and copy machinery. It maps only its existing float32, uint32 and uint8 storage formats, charges the complete retained allocation, and validates typed exports before allocation. `toCpu<Rank>` remains the float32 interface; private typed exports carry an explicit element. Other CPU formats are rejected by this bridge until their Metal storage support is qualified. Upload/download can preserve existing integer storage buffers, but this does not enable integer shaders or source arithmetic. Resident values still require explicit download.

[CPU storage tests](../../native/tests/CpuStorageTests.cpp) cover all ten types, descriptor layouts, zero/rank-zero tensors, offset/alignment/stride/bounds checks, capture/result lifetimes, allocation failures and scalar task results with 1/2/4 workers. [Metal adapter tests](../../native/metal/CpuStorageTests.cpp) cover typed float32/uint32/uint8 round trips, zero-copy leases, full-allocation charging, unsupported formats, missing tags, width mismatches and explicit residency. Both run in debug, optimized, ASan/UBSan and TSan builds. Existing compiled float32 interoperability and complete numerical/task lifecycle checks remain required. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-storage-2026-10-16.json) records those checks.

Checked CPU int32 scalar arithmetic is now implemented below. Tensor arithmetic, additional types and explicit conversions require their own lowering and qualification before enabling their corresponding capability gates. The shared runtime storage does not authorize reusing floating-point instructions or float32 numerical proofs for integer computation. Distributed execution remains behind its separate design review.

## Checked CPU int32 scalar arithmetic

Explicit `int32` annotations now work on ordinary scalar parameters, results and immutable bindings:

```kalandra
fn increment(value: int32) -> int32:
  value + 1

fn main() -> int32:
  initial: int32 = 16777217
  increment(initial)
```

The executable prints `16777218` exactly. Integer function calls and CLI entries use integer registers and results; they never pass through float32. The existing float32 CLI and tensor ABI are preserved.

Contextual decimal integer literals cover `-2147483648` through `2147483647`. Signed minimum is parsed with its unary sign, without requiring the positive magnitude to fit int32 first. Out-of-range and fractional/exponent literals in an int32 context are rejected with S051. Unconstrained whole-number literals now use the int64 implementation qualified below; unconstrained floating literals still need float32 context until float64 arithmetic is available. Literals do not silently default to int32. Typed int32 and float32 values cannot mix implicitly.

Unary `+`/`-` and binary `+`, `-`, `*`, `/`, `%` retain int32. The compiler diagnoses known invalid arithmetic in literal expressions and immutable constant bindings with S052, including a known zero divisor with a runtime numerator. It does not perform interprocedural constant evaluation. Operations on runtime values check overflow and division/remainder by zero in every build. An invalid operation records `ArithmeticFault` at its source span and returns through the existing checked invocation convention. It ends the current task rather than resuming the invalid operation, wrapping, or depending on debug assertions. A supervised child fault is exposed to its parent as TaskError.Fault at the task boundary, as described below.

MLIR lowering sign-extends int32 operands to int64, computes without overflowing that wider representation, checks the int32 result bounds, then narrows. Division/remainder check a zero divisor before execution. Widened intermediates implement the signed-minimum cases safely: `-2147483648 / -1` faults and `-2147483648 % -1` is zero. Negative quotient/remainder use truncation toward zero. Fault propagation stops dependent function calls and preserves the invocation cleanup path.

The initial scalar increment covered synchronous CPU scalars and ordinary functions. Int32 CPU task continuations are now qualified below. Int32 synchronous tensor literals and elementwise operations are now qualified below. Integer tensor task handoff is now qualified below. Int32 matmul is now qualified below. Metal modules/kernels, conversions and other numeric arithmetic types remain unsupported and diagnosed. Type metadata and runtime storage availability do not imply those capabilities. Tensor and GPU arithmetic follow separate qualification. Distributed execution still requires the agreed design review.

[Int32 frontend tests](../../native/tests/Int32Tests.cpp) cover literal boundaries, immutable constants, context/default rules, type mismatches, typed calls and unsupported target paths. The frontend runs independently under ASan/UBSan so its container instrumentation is not mixed with the uninstrumented MLIR backend. [Runtime conformance tests](../../native/tests/Int32RuntimeTests.cpp) compare six operations against wider integer reference arithmetic over 10,361 boundary/random input pairs per operation in debug, optimized, ASan/UBSan and TSan builds. Generated wrappers forward through an ordinary function to verify fault propagation. CLI checks cover exact endpoints, values beyond float32 precision, arithmetic fault reporting, output preservation on static rejection and Metal rejection. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-2026-10-16.json) records the results and unchanged float32 numerical artifacts.

## Int32 CPU task captures and results

CPU `spawn`/`await` and structured parallel groups now support int32 scalar arguments and results, including independently scheduled nested helpers, direct calls, aliases and repeated awaits. Int32 and float32 parameters/results can coexist in a module and in one capture packet; this does not permit implicit arithmetic conversions. Either numeric type can be the scalar executable result.

```kalandra
fn increment(value: int32) -> int32:
  value + 1

fn main() -> int32 ! TaskError:
  try parallel:
    work = spawn increment(16777217)
    try await work
```

The compiler carries int32 through continuation slots, frame fields, capture packets, cached task results and the root entry. Generated result wrappers contain the existing failure header plus a typed payload. Their private int32 and float32 layout identities and copy functions are distinct even though the payload widths match. These generated wrappers are separate from the raw numeric scalar adapter layouts described above; neither is a distributed wire format.

Integer task arithmetic is lowered to synchronous numerical steps by the common continuation planner. Those steps use the existing checked int32 MLIR/LLVM lowering, including safe division/remainder and overflow checks. The task emitter does not implement another set of integer operators. Function-call overhead in this initial lowering is not a performance optimization claim. Existing scheduling, cancellation, ownership and group settlement remain shared with float32 tasks.

A fault stops the executing task and retains its original source location. At the supervised child boundary it becomes `TaskError.Fault` with `FaultKind.Arithmetic` and an arithmetic diagnostic message. An outer handler may inspect the settled group's error and continue with a new result; it cannot resume the failed operation. An uncaught supervised failure prints TaskError.Fault at the CLI boundary. An arithmetic fault in the coordinating task itself remains an ArithmeticFault. Allocation, native and admission errors keep their existing classification.

[Task conformance checks](../../native/cmake/VerifyInt32Tasks.cmake) run compiled programs in debug, optimized, ASan/UBSan and TSan modes with 1/2/4 workers. They cover signed endpoints, values above float32's exact precision, all six checked operations, negative quotient/remainder, signed-minimum remainder -1, mixed scalar packets/results, both numeric root types, nested scheduling, cached awaits, child faults and recovery, original source locations, sibling tensor ownership, parent data after failure, root allocation failure and invalid worker admission. [Fixtures](../../native/tests/fixtures/task-int32-scalars.k) and [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-tasks-2026-10-16.json) accompany the full CPU/Metal regression.

CPU int32 tensor literals, elementwise operations and tensor task captures/results are now qualified below. Checked int32 reduceSum is now qualified below. Int32 matmul is qualified below; GPU arithmetic, additional numeric families and conversions need their own follow-up checks. Distributed execution remains behind the agreed design review.

## Checked CPU int32 tensor literals and elementwise operations

Synchronous CPU functions now accept and return `Tensor[int32,...]`, with fixed or dynamic extents. Tensor literals type each leaf as int32, including signed minimum and exact values above float32 precision. Nested rectangular data and explicit constant flat-data shapes use the existing shape checks; rank-zero and empty tensors remain supported. An int32 tensor context is required, and float32 values are not implicitly converted into integer elements.

```kalandra
fn shift(values: Tensor[int32, ?], amount: int32) -> Tensor[int32, ?]:
  values + amount

fn example() -> Tensor[int32, 3]:
  values: Tensor[int32, 3] = tensor([16777217, -5, 9])
  shift(values, 1)
```

Unary `+`/`-` and elementwise `+`, `-`, `*`, `/`, `%` preserve int32. A scalar may appear on either side of an integer tensor operation. Two tensors require matching ranks and extents; this does not add broadcasting. Runtime shape guards run before element evaluation. Empty tensors evaluate no element operations, so an empty tensor divided by scalar zero produces an empty tensor rather than a division fault.

Each evaluated element uses the same checked int32 MLIR helpers as ordinary scalar arithmetic. Overflow, division/remainder by zero, and signed-minimum division are checked in every build. Signed-minimum remainder -1 is zero. A failure retains the operation's source span, stops the invocation through the checked ABI, and publishes no result. Invocation ownership settles any partially constructed buffers. Inputs remain immutable, including aliased operands and borrowed native descriptor views with offsets. Literal ranges and scalar constant expressions retain their compile-time checks; this increment does not introduce aggregate tensor constant folding or interprocedural evaluation.

Tensor element identity now reaches ranked tensor/memref types, literal construction, result allocation and private checked native interfaces. Byte-count checks remain valid for the two supported four-byte element formats. Int32 reduceSum and matmul are qualified below with separate checked integer paths; backend admission also checks matching operand/result types. Integer tensor task captures/results are now qualified below with distinct element-aware layouts and retained ownership. GPU modules/kernels remain float32-only. This increment does not add indexing, conversions, a public foreign-memory API or a new tensor-valued CLI print format.

[Runtime conformance checks](../../native/tests/Int32TensorRuntimeTests.cpp) compare exact cells against independent wider integer arithmetic. The [compiled test driver](../../native/cmake/VerifyInt32Tensors.cmake) covers all binary operations with tensor/tensor and both scalar orders, unary operations, native input aliases/offsets, nontrivial ranked shapes, every empty dimension, dynamic-to-fixed shape refinement, signed endpoints, deterministic random inputs, late element faults, invalid dimensions/byte counts, allocation failure, unpublished fault results and balanced cleanup. Literal checks cover ranks zero through three. All run in debug, optimized, ASan/UBSan and TSan modes. [Frontend checks](../../native/tests/Int32Tests.cpp) also reject malformed literals, mixed element types, ragged/mismatched shapes, mixed-element/mismatched matmul and invalid private test exports. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-tensors-2026-10-16.json) records the complete CPU/Metal regression and numerical artifact comparison.

Typed int32 tensor task captures/results are now qualified below. Checked int32 reduceSum is qualified below; int32 matmul is qualified below and GPU integer arithmetic remains a later increment. Distributed execution still requires its separate design review.

## Int32 tensor captures and results in CPU tasks

CPU workers now accept and return int32 tensors, including rank-zero values, empty tensors, nested coordinating helpers and mixed int32/float32 capture packets. Awaiting or returning a tensor retains its immutable backing through the existing invocation ownership machinery. Cached awaits can acquire independent owners of the same backing; buffers are not reconstructed through float32 or copied through a floating-point value representation.

```kalandra
fn shift(values: Tensor[int32, ?], amount: int32) -> Tensor[int32, ?]:
  values + amount

fn batch(values: Tensor[int32, ?]) -> Tensor[int32, ?] ! TaskError:
  try parallel:
    work = spawn shift(values, 1)
    try await work
```

Generated tensor result layouts now include numeric element identity and rank in their private keys/IDs. Int32 and float32 tensors therefore use distinct layout objects even when their memref descriptors have identical physical sizes. The scheduler's existing exact-layout check rejects mismatched observations before copying or retaining data. These compiler/runtime descriptors remain internal ABI metadata, not a distributed serialization format.

Capture construction, publication and observation reuse the existing retain/copy protocol. A successful handoff retains its dependencies before publishing the descriptor. The receiving invocation can keep the value after all workers, task frames, intermediate outcomes and the core itself have been destroyed. An independent immutable lease can then outlive the receiving invocation. A failed capture or observation publishes no tensor and follows the existing allocation/fault settlement path.

Integer tensor arithmetic still uses the checked synchronous MLIR lowering. A child arithmetic fault keeps its source span, becomes TaskError.Fault/FaultKind.Arithmetic at the supervision boundary and cancels/settles the affected group. Handlers can use the original parent tensor and start another group. Shape refinement and failure propagation avoid reading absent tensor payloads. Root/child allocation failures, quota admission rejection and explicit cancellation/deadline signals retain their existing classification. This step adds no new deadline timer or scheduling policy.

The compiler's existing private `--test-entry` option can now select a parameterless numeric function in a task module. It exports a const TaskEntry pointer, `kalandra_task_test_entry`, so native tests can drive the compiled code through TaskCore and inspect its wrapped result. This is a trusted test interface; arbitrary task inputs and a public foreign-memory API are not introduced. Ordinary executable main functions still return scalar int32 or float32; tensor CLI formatting is unchanged.

[Compiled ownership tests](../../native/tests/Int32TensorTaskTests.cpp) inspect exact returned cell bits, source faults, distinct same-width layouts and scheduler counters. They destroy every worker and task owner, then the receiving invocation, while checking the independently retained result. [Fixtures](../../native/tests/fixtures/task-int32-tensor-mixed.k) and the [test driver](../../native/cmake/VerifyInt32TensorTasks.cmake) cover signed endpoints, values above float32 precision, rank-zero through rank-three results, empty tensors, mixed element families, nested/cached awaits, arithmetic recovery, runtime shape failure without an absent-payload read, twelve allocation-failure positions, byte-budget admission rejection and explicit cancellation/deadline settlement. They run with 1/2/4 workers in debug, optimized, ASan/UBSan and TSan variants. The optimized observer also links the existing optimized task core. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-tensor-tasks-2026-10-16.json) records the full CPU/Metal regression and unchanged numerical artifacts.

Checked CPU int32 reduceSum is qualified below. Int32 matmul is qualified below; additional numeric types, conversions and GPU integer arithmetic remain separate increments. Distributed execution still requires its agreed design review.

## Checked CPU int32 reduceSum

`reduceSum` accepts a CPU `Tensor[int32, ...]` of any supported rank and returns an `int32`. A rank-zero tensor returns its single element. Any zero extent gives the empty identity `0`.

```kalandra
fn sum(values: Tensor[int32, ?]) -> int32:
  reduceSum(values)

fn batch(values: Tensor[int32, ?]) -> int32 ! TaskError:
  try parallel:
    try await spawn sum(values)
```

The operation uses the accepted row-major, balanced-adjacent tree: sum neighboring pairs, carry an odd final element unchanged, then repeat on the resulting values. Each addition sign-extends to int64, checks the int32 range, then narrows. For example, `[2147483647, 1, -1]` faults at its first pair even though its mathematical total fits. Conversely, `[2147483647, -1, 1]` succeeds. This is not a widened-total or sequential-fold contract. The compiler reuses the existing tree lowering with integer scratch storage and checked integer additions; float32 retains its previous tree and arithmetic.

An overflow raises an execution Arithmetic fault at the reduceSum source location, with no successful scalar result. A child fault is supervised as `TaskError.Fault` with `FaultKind.Arithmetic`; handlers can reuse the unchanged input and start another parallel group. Scratch allocations belong to the invocation and settle after success, overflow, allocation failure or cancellation. Generated loops retain cooperative cancellation polling. Empty reductions perform no element loads or additions; allocation may still fail while constructing internal scratch storage.

[Runtime reference tests](../../native/tests/Int32ReductionRuntimeTests.cpp) compare ranks zero through three against a separate wider-integer adjacent-tree reference. They cover signed endpoints, values above float32 precision, deterministic random inputs, every empty axis, odd tails, multi-level trees, intermediate overflow despite an in-range total, borrowed offsets/noncontiguous strides, invalid dimensions/byte counts, allocation failure, cancellation and balanced cleanup. The [compiled conformance driver](../../native/cmake/VerifyInt32Reductions.cmake) also exercises nested worker calls, cached awaits, tensor siblings, exact scalar returns, Arithmetic recovery with parent tensor reuse, original source locations and uncaught fault cleanup with 1/2/4 workers. Tests run in debug, optimized, ASan/UBSan and TSan modes. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-reductions-2026-10-16.json) records the full regression and unchanged float32 numerical artifacts.

Checked CPU int32 matmul is qualified below. GPU integer arithmetic, additional numeric types and conversions remain separate increments. Distributed execution still requires the agreed design review.

## Checked CPU int32 matmul

`matmul` accepts two CPU int32 matrices shaped `[M,K]` and `[K,N]`, and returns a fresh immutable `Tensor[int32,M,N]`. Known inner-dimension mismatches are diagnosed; dynamic mismatches fault before element access. Both operands must have the same numeric element type.

```kalandra
fn project(input: Tensor[int32, ?, ?], weights: Tensor[int32, ?, ?]) -> Tensor[int32, ?, ?]:
  matmul(input, weights)
```

Each output cell computes products in ascending contraction-index order. Every product must fit int32 before entering the balanced-adjacent sum tree, and every addition in that tree must also fit int32. Multiplication and addition use int64 intermediates solely to check the int32 result, then narrow; there is no wider accumulator, wrapping arithmetic or fused multiply-add. Overflow raises Arithmetic at the matmul source location, even when later terms would cancel it. A fault in a later cell publishes no partial matrix.

The initial integer path uses serial row/column loops and one invocation-owned int32 scratch buffer reused for each cell. It reuses the qualified reduction tree and checked scalar operations. It does not use the float32 SIMD/tiled kernels, and this qualification makes no performance claim. Inputs, including aliased/noncontiguous borrowed views, remain immutable. Successful returned tensor ownership follows the existing checked interfaces and task handoff rules.

Zero output rows or columns give an empty matrix without sizing scratch from the unused contraction dimension or entering contraction loops. A zero contraction extent produces a matrix of integer zeros. The private native boundary still rejects negative dimensions and input/output byte-count overflow, including invalid input descriptors when no output would be computed. Scratch/result allocations can fail even for empty operations. Overflow, allocation failure and cooperative cancellation retain invocation cleanup; nested task failures use the existing supervised Arithmetic classification and preserve source locations.

[Runtime conformance tests](../../native/tests/Int32MatmulRuntimeTests.cpp) compare exact cells against independently computed int64 products and an adjacent-pair reference tree. They cover endpoints and values above float32 precision, deterministic random shapes/contents, noncontiguous/offset and transposed views, operand aliases, empty contractions/outputs, odd and multi-level trees, product/pair/later-level overflow, late-cell failure, invalid dimensions and byte counts, huge unused contraction extents in valid empty descriptors, allocation failure, retained results and cancellation. The [compiled test driver](../../native/cmake/VerifyInt32Matmul.cmake) adds fixed-shape results, synchronous CLI faults, nested/cached worker results, returned tensors surviving worker/core/invocation destruction, allocation/admission/cancellation/deadline signals, and Arithmetic recovery with unchanged parent data and a new parallel group. Tests run in debug, optimized, ASan/UBSan and TSan modes with 1/2/4 workers. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int32-matmul-2026-10-16.json) records the full CPU/Metal regression and unchanged float32 numerical artifacts.

Checked CPU uint32 scalar arithmetic is qualified below. GPU integer arithmetic and numeric conversions remain separate qualification work. Distributed execution still requires the agreed design review.

## Checked CPU uint32 scalar arithmetic

The native CPU subset now accepts `uint32` scalar literals, immutable bindings, ordinary functions, native scalar interfaces and structured worker calls. Values range from 0 through 4294967295. There is no implicit conversion between uint32, int32 and float32, and unconstrained literals retain the accepted int64/float64 defaults; the int64 default is now qualified below, while float64 remains outside this executable subset. `uint` remains the uint64 alias rather than changing width to fit the implementation.

```kalandra
fn quotient(value: uint32, divisor: uint32) -> uint32:
  value / divisor

fn main() -> uint32 ! TaskError:
  try parallel:
    try await spawn quotient(4294967295, 2)
```

Unary `+` and binary `+`, `-`, `*`, `/`, `%` preserve uint32. Addition/multiplication overflow, subtraction underflow and zero divisors fault in every build mode. Negation preserves the type: `-0` is zero; negating a positive uint32 overflows rather than wrapping or becoming signed. Known invalid constants and immutable constant bindings are diagnosed at compilation. The constant evaluator checks a multiplication bound before evaluating a product that could exceed signed int64, so even `4294967295 * 4294967295` is safely rejected.

MLIR lowering zero-extends operands to 64 bits. Addition and multiplication use that full unsigned representation, check the result against UINT32_MAX and narrow to i32 storage. Subtraction checks operand order before subtracting. Division/remainder use unsigned instructions after a zero-divisor check, so values above INT32_MAX are never interpreted as negative. Unary negation checks for zero. The dedicated uint32 CLI entry prints exact unsigned decimal values; signed/float entries keep their previous ABI and formatting.

The common continuation planner carries explicit UInt32 literals and sends unsigned arithmetic through the same checked MLIR path. Uint32, int32 and float32 scalar task results use distinct generated keys/IDs despite equal storage widths. Nested scheduling, cached awaits and mixed scalar/tensor captures reuse existing ownership and scheduling. A child Arithmetic fault preserves its source location and is supervised through TaskError.Fault; handlers may reuse parent tensors and start a new parallel group.

[Frontend checks](../../native/tests/UInt32Tests.cpp) cover literal/constant limits, negative/fractional values, unsigned negation, division by known zero, rejected implicit conversions and the uint64 default alias. [Runtime reference tests](../../native/tests/UInt32RuntimeTests.cpp) compare seven operations with unsigned 64-bit reference arithmetic over 10,256 boundary/random input pairs per operation. The [compiled driver](../../native/cmake/VerifyUInt32.cmake) qualifies synchronous forwarding, full-range CLI values, successful worker arithmetic, uint32/int32/float32 roots and mixed captures, nested/cached awaits, all six faulting operations, Arithmetic recovery, source locations, admission/allocation cleanup and output preservation on static errors. Frontend checks run independently under ASan/UBSan; generated programs run in debug, optimized, ASan/UBSan and TSan modes, with task cases at 1/2/4 workers. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint32-2026-10-16.json) records the full CPU/Metal regression and unchanged float32 numerical artifacts.

Uint32 tensor literals and checked synchronous elementwise operations are now qualified below. Uint32 tensor task handoff, reductions and matmul are qualified below; GPU arithmetic and numeric conversions remain gated. This increment adds no public foreign-memory/wire ABI and no distributed execution. Distributed execution still requires the agreed design review.

## Checked CPU uint32 tensor literals and elementwise operations

The synchronous CPU subset now accepts immutable uint32 tensor literals, ordinary tensor-valued functions and elementwise unary/binary operations. Literals retain their exact integer bits, including UINT32_MAX and values above INT32_MAX or float32 precision. Ranks zero through three and explicit/nested literal shapes have compiled conformance coverage; rank-zero tensors contain one element and a zero extent gives an empty tensor. Existing rank/size bounds still apply.

```kalandra
fn shift(values: Tensor[uint32, ?], amount: uint32) -> Tensor[uint32, ?]:
  values + amount
```

Unary `+`/`-` and binary `+`, `-`, `*`, `/`, `%` preserve uint32. Scalars can appear on either side of a tensor operation; two tensor operands require equal ranks and extents. There is no implicit conversion or broadcasting. Each evaluated lane uses the same zero-extension, unsigned division/remainder, underflow/overflow and negation checks as scalar uint32. Empty tensors perform no element arithmetic, so a zero divisor or negation has no failing lane in an empty result. A zero divisor shared across nonempty lanes still faults.

Numeric identity is carried separately from MLIR's signless i32 element representation. Elementwise lowering explicitly selects signed or unsigned checks from typed IR; matching storage widths never select an arithmetic interpretation. Shapes, result allocation, checked native interfaces and literal construction reuse the existing element-aware paths.

On an Arithmetic fault, the source operation location is preserved and no partial tensor is published. Inputs remain immutable, including aliased operands and borrowed views with noncontiguous strides/offsets. Invocation-owned result storage settles after success, arithmetic/shape failure, allocation failure or cooperative cancellation. A retained immutable allocation lease can preserve a successful result after invocation settlement. This remains a private native test/interface capability, without a new public foreign-memory ABI or tensor-valued CLI print format.

[Compiled runtime reference tests](../../native/tests/UInt32TensorRuntimeTests.cpp) use separate uint64 arithmetic to compare exact cells. The [driver](../../native/cmake/VerifyUInt32Tensors.cmake) exercises tensor/tensor and both scalar orders for five binary operators, unary identity/negation, literal ranks zero through three, dynamic-to-fixed shape refinement, signed/float precision boundaries, deterministic random inputs, nontrivial shapes, every empty axis, aliases, offset/noncontiguous input views, late failing lanes, invalid dimension/byte counts, allocation failure, retained results, cancellation and balanced cleanup. All run in debug, optimized, ASan/UBSan and TSan modes. [Frontend tests](../../native/tests/UInt32Tests.cpp) reject out-of-range/negative/ragged literals and mixed numeric element types. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint32-tensors-2026-10-16.json) records the full CPU/Metal regression and unchanged float32 numerical artifacts.

Uint32 tensor handoff through CPU tasks is now qualified below. GPU uint32 arithmetic remains rejected. Uint32 reductions and matmul are qualified below; static matmul shape/type errors preserve existing executable/IR files.

Uint32 tensor task captures/results are qualified below. Reductions and matmul are qualified below; GPU integer arithmetic remains a later increment. Distributed execution still requires the agreed design review.

## UInt32 tensor captures and results in CPU tasks

CPU tasks now capture and return immutable uint32 tensors, including nested coordinators, cached awaits, rank-zero and empty results. Same-rank uint32, int32 and float32 values may share a capture packet while retaining separate numeric identities. Values remain exact through handoff, including UINT32_MAX and values above signed-int32/float32 precision boundaries.

```kalandra
fn shift(values: Tensor[uint32, ?], amount: uint32) -> Tensor[uint32, ?]:
  values + amount

fn batch(values: Tensor[uint32, ?]) -> Tensor[uint32, ?] ! TaskError:
  try parallel:
    work = spawn shift(values, 1)
    try await work
```

Generated uint32 tensor result keys and private layout IDs include the unsigned element identity and rank. Int32/float32 retain their existing keys and layouts; identical descriptor sizes do not merge identities. The existing exact-layout check rejects mismatched observations before copying or retaining data. These layouts remain compiler/runtime metadata, without introducing a wire format or public foreign-memory ABI.

Capture construction and result observation reuse the existing retain/copy protocol. Returned storage can outlive all worker threads, task records, intermediate outcomes and the core, and a retained immutable lease can outlive its receiving invocation. Repeated awaits/observations acquire valid independent ownership. No scheduler, runtime ownership or checked invocation ABI changes are needed.

Checked unsigned arithmetic continues through the existing MLIR kernel path. Overflow, underflow, division/remainder by zero and positive-value negation become supervised Arithmetic faults with their source location. A failed result exposes no tensor payload. Recovery may reuse the unchanged parent tensor, then start a new parallel group. Allocation and quota admission failures, explicit cancellation/deadline signals and dynamic shape failures retain existing settlement and classification. This does not add a physical deadline timer.

The [compiled driver](../../native/cmake/VerifyUInt32TensorTasks.cmake) uses the [shared native result observer](../../native/tests/Int32TensorTaskTests.cpp) with unsigned, signed and floating result types. [Mixed packets](../../native/tests/fixtures/task-uint32-tensor-mixed.k), [nested temporaries](../../native/tests/fixtures/task-uint32-tensor-nested.k) and [fault recovery](../../native/tests/fixtures/task-uint32-tensor-recovery.k) exercise exact cells, distinct layouts, ranks zero through three, empty tensors, wrong-layout rejection, cached outcomes, owner survival after core/invocation destruction, all scheduler live counters returning to zero, absent payloads on faults, original source locations and twelve allocation-failure positions. Tests run in debug, optimized, ASan/UBSan and TSan modes with 1/2/4 workers. The signed test variants also run through the same observer. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint32-tensor-tasks-2026-10-16.json) records the complete CPU/Metal regression and unchanged float32 numerical artifacts.

Checked CPU uint32 reduceSum is qualified below. Uint32 matmul is qualified below; GPU integer arithmetic, further numeric types and conversions remain separate increments. Distributed execution still requires the agreed design review.

## Checked CPU uint32 reduceSum

`reduceSum` accepts a CPU uint32 tensor of any supported rank and returns an exact uint32 scalar. The reduction uses the existing row-major balanced-adjacent tree: add neighboring pairs and carry an odd tail unchanged. A rank-zero tensor returns its one element; empty input returns zero. Inputs and results preserve the full unsigned range, including values above INT32_MAX and float32 precision. There are no implicit numeric conversions.

```kalandra
fn sum(values: Tensor[uint32, ?]) -> uint32:
  reduceSum(values)

fn main() -> uint32 ! TaskError:
  values: Tensor[uint32, 3] = tensor([2147483648, 2147483647, 0])
  try parallel:
    try await spawn sum(values)
```

This returns `4294967295`. Each tree addition zero-extends to uint64, checks against UINT32_MAX and narrows to uint32. Overflow faults in every build mode, rather than wrapping or widening the result. For example, `[4294967295, 1, 0]` faults at the first pair and `[4294967295, 0, 1, 0]` faults when the pair results are combined. Unlike signed inputs, nonnegative unsigned inputs cannot have an overflowing partial sum and then cancel back into range.

The reduceSum source location is retained on failure. Worker overflow becomes `TaskError.Fault` with `FaultKind.Arithmetic`, without publishing a successful scalar. Recovery can reuse the unchanged parent tensor and start another parallel group. Existing invocation-owned scratch storage, cancellation polling, shape/byte-count checks and cleanup apply to successful, overflowed, cancelled and allocation-failed calls. This increment changes no scheduler, continuation planner or public memory ABI.

The implementation reuses the same reduction scratch/tree lowering and checked unsigned addition as existing tensor arithmetic. The [compiled conformance driver](../../native/cmake/VerifyInt32Reductions.cmake) and [runtime reference harness](../../native/tests/Int32ReductionRuntimeTests.cpp) now specialize by integer type instead of copying the signed suite. Unsigned references use independent uint64 arithmetic, including full-range random inputs, UINT32_MAX, values above signed and float32 boundaries, ranks zero through three, empty axes, odd tails, large trees and late overflow. Borrowed offsets/strides, immutable inputs, invalid metadata, allocation failure, cancellation, nested/cached worker results, recovery, exact CLI output and source locations are checked in debug, optimized, ASan/UBSan and TSan builds, with task cases at 1/2/4 workers. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint32-reductions-2026-10-16.json) records the results.

Additional numeric types reuse these mechanisms, but each needs conformance checks for its own range, representation and arithmetic rules. Wider integers need suitable checked intermediates or overflow instructions; additional float formats need their own rounding and edge-case qualification. Shared algorithms and test harnesses do not imply automatic backend support.

Unsigned matmul is qualified below; GPU integer arithmetic and numeric conversions remain gated. Distributed execution still requires the agreed design review.

## Checked CPU uint32 matmul

`matmul` accepts two CPU uint32 matrices shaped `[M,K]` and `[K,N]`, returning a fresh immutable `Tensor[uint32,M,N]`. Both operands must have the same element type. Known inner-dimension mismatches are compile errors; dynamic mismatches fault before element access. Integer and floating-point matrices never implicitly convert into each other.

```kalandra
fn project(input: Tensor[uint32, ?, ?], weights: Tensor[uint32, ?, ?]) -> Tensor[uint32, ?, ?]:
  matmul(input, weights)

fn main() -> uint32 ! TaskError:
  input: Tensor[uint32, 1, 2] = tensor([[2147483648, 2147483647]])
  weights: Tensor[uint32, 2, 1] = tensor([[1], [1]])
  try parallel:
    work = spawn project(input, weights)
    result = try await work
    reduceSum(result)
```

This returns `4294967295`. Each product zero-extends its operands to uint64, checks against UINT32_MAX and narrows to uint32. The uint64 product can represent every pair of uint32 operands exactly. Each output cell then uses the same checked balanced-adjacent addition tree as uint32 reduceSum, carrying an odd tail unchanged. A product or any tree addition outside the uint32 range faults in every build mode; the result does not wrap or accumulate into a wider result type.

The integer kernel reuses existing shape/byte-count checks, scratch allocation, row/column loops, cancellation polling and immutable result storage. A zero contraction dimension produces zero-filled output. Zero output rows or columns perform no contraction work or contraction-sized scratch allocation, while input metadata still passes the existing bounds checks. Borrowed offsets and noncontiguous/transposed strides are respected, including aliased operands.

A failure retains the original matmul source location and publishes no partial matrix, including when an earlier output cell succeeded. Worker failures become `TaskError.Fault`/`FaultKind.Arithmetic`. Recovery can reuse parent inputs and start another group. Nested coordinators and cached awaits retain results through the existing typed capture/result layouts; an independent lease can outlive the task core and receiving invocation. Allocation/quota failures and explicit cancellation/deadline signals use existing settlement rules. This adds no new scheduler, deadline timer or public foreign-memory ABI.

The [shared integer runtime harness](../../native/tests/Int32MatmulRuntimeTests.cpp) now specializes signed and unsigned cells and compares products/tree sums with independent wider arithmetic. It covers unsigned endpoints, values above signed/float32 boundaries, full-range deterministic random inputs, UINT32_MAX times UINT32_MAX, products around 65536 squared, first-pair/later-tree overflow, a fault after a successful output cell, empty contractions/output shapes, large odd contraction sizes, padded/transposed views, aliases, invalid shapes/byte counts, allocation failure, cancellation and retained output after invocation settlement.

The [shared compiled driver](../../native/cmake/VerifyInt32Matmul.cmake) also checks fixed shapes, exact full-range CLI output, nested workers, cached results, independent ownership after task/core/invocation destruction, twelve allocation-failure positions, admission/cancellation/deadline settlement, and Arithmetic recovery for product, first-pair and later-tree faults. Static shape/type/rank errors preserve existing output artifacts; forged mismatched element types are rejected by the backend. Debug, optimized, ASan/UBSan and TSan variants run with 1/2/4 workers. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint32-matmul-2026-10-16.json) records the regression and numerical artifact comparison.

Further integer widths should extend the shared numerical policies and conformance matrix. GPU integer arithmetic, numeric conversions and broader language values remain separate increments. Distributed execution still requires the agreed design review.

## Checked CPU int64 scalar arithmetic

The CPU subset now supports signed int64 scalar literals, immutable bindings, ordinary functions, private native scalar interfaces and structured worker calls. Values range from -9223372036854775808 through 9223372036854775807. The accepted `int` alias has exactly this type, and unconstrained whole-number literals now infer int64. An expected int32, uint32 or float32 type still types a literal directly; an already typed int64 value never silently narrows or converts.

```kalandra
fn increment(value: int) -> int64:
  value + 1

fn main() -> int ! TaskError:
  start = 9007199254740993
  try parallel:
    try await spawn increment(start)
```

This prints `9007199254740994` exactly. Unconstrained decimal/exponent literals still require float32 context until float64 arithmetic is qualified. `uint` remains uint64 and is now qualified below. Int64 tensors, reductions and matmul are qualified below; GPU integer arithmetic and numeric conversions remain gated.

Unary `+`/`-` and binary `+`, `-`, `*`, `/`, `%` preserve int64. Overflow and zero divisors are checked in every build mode. Division truncates toward zero; INT64_MIN divided by -1 faults, while INT64_MIN remainder -1 is zero. Signed minimum parses with its leading minus, without requiring its positive magnitude to fit first. Known invalid literals, arithmetic and constant bindings are diagnosed before code generation. Compile-time calculations use a wider signed representation so the compiler itself never overflows while checking these operations.

Signed MLIR lowering is now width-aware. Int64 addition, subtraction, multiplication and negation sign-extend into i128, check the int64 bounds and narrow; existing int32 operations retain their i64 intermediate. Division uses guarded i64 arithmetic, without a widened software division. Remainder substitutes a safe divisor for the INT64_MIN/-1 case to avoid LLVM poison while producing the mathematical zero. The new CLI entry returns and prints exact signed 64-bit values; existing float32/int32/uint32 entries retain their ABI.

The common continuation planner sends int64 arithmetic through the checked kernel path. Task captures/results use i64 storage and distinct private scalar layout keys/IDs. Mixed packets can contain int64, int32, uint32 and float32 simultaneously, preserving their widths, alignment and interpretation. Nested workers, cached awaits, tensor-owning siblings and Arithmetic recovery reuse the existing scheduler and ownership machinery. Child faults preserve source location and classify as TaskError.Fault/Arithmetic; root coordinating faults settle the group before reporting failure. This adds no new runtime ownership or checked invocation ABI.

[Frontend checks](../../native/tests/Int64Tests.cpp) cover endpoints, values above float64 integer precision, signed minimum parsing, constant overflow/zero division, the int alias, default inference and rejected conversions/tensor/GPU capabilities. The [shared signed scalar driver](../../native/cmake/VerifyInt32.cmake), [width specialization](../../native/cmake/SignedScalarType.cmake) and [runtime harness](../../native/tests/Int32RuntimeTests.cpp) compare all six nontrivial operators against independent wider arithmetic over boundaries and deterministic full-range inputs. Exact endpoints and CLI output, native-width division/remainder, source locations and artifact preservation on static errors are checked. The [shared worker driver](../../native/cmake/VerifyInt32Tasks.cmake) checks mixed-width/family captures with each supported root type, nested/cached calls, every arithmetic failure, recovery with owned parent data, root admission/allocation rejection and zero-live-allocation cleanup at 1/2/4 workers. Generated programs run in debug, optimized, ASan/UBSan and TSan builds; frontend checks also run independently under ASan/UBSan. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int64-scalars-2026-10-16.json) records the regression and numerical artifact comparison.

Checked CPU uint64 scalars are now qualified below. Int64 tensors and further numeric formats remain separate increments. Distributed execution still requires the agreed design review.

## Checked CPU uint64 scalar arithmetic

The CPU subset now supports unsigned uint64 scalar literals, immutable bindings, ordinary functions, private native scalar interfaces and structured worker calls. The `uint` alias has exactly this type, with values from 0 through 18446744073709551615. Whole-number literals without context still infer signed int64; use uint/uint64 context for values above INT64_MAX. Typed integers never implicitly widen, narrow or switch signedness.

```kalandra
fn identity(value: uint) -> uint64:
  value

fn main() -> uint ! TaskError:
  value: uint = 18446744073709551615
  try parallel:
    try await spawn identity(value)
```

This prints `18446744073709551615` exactly. Unary `+`/`-` and binary `+`, `-`, `*`, `/`, `%` preserve uint64. Addition/multiplication overflow, subtraction underflow and zero divisors fault in every build mode. Negation succeeds only for zero. There are no implicit conversions to int64, narrower integers or float32, even when a particular value would fit.

Known invalid literals, constant arithmetic and immutable constant bindings are rejected before code generation. Constant storage now preserves the full uint64 range. The compiler checks the multiplication bound before evaluating an unsigned product, so UINT64_MAX times UINT64_MAX is safely diagnosed rather than overflowing the compiler's signed intermediate. Intermediate overflow still faults even if a later operation would bring the mathematical total back into range.

The shared unsigned MLIR path is width-aware. Addition, subtraction and multiplication zero-extend to i128, check uint64 bounds and narrow; every possible pair of uint64 operands has an exact product in unsigned i128. Subtraction checks operand order first. Division and remainder use guarded native-width unsigned i64 instructions, without widened software division. Negation checks zero at the operand's width. These are compiler intermediates, not new source int128/uint128 types.

The dedicated CLI entry preserves exact unsigned decimal output. The common continuation planner carries UInt64 literals and checked kernels; generated task layouts distinguish uint64 and int64 despite identical 8-byte storage. Captures may mix uint64, int64, uint32, int32, float32 and existing float32 tensor values. Nested workers, cached awaits, root admission/allocation rejection and Arithmetic recovery reuse the existing scheduler and ownership machinery. Child faults retain source locations and publish no successful scalar result; recovery can reuse parent tensors and start another group. Existing 32-bit and signed64 entries retain their ABI.

[Frontend checks](../../native/tests/UInt64Tests.cpp) cover endpoints, the uint alias, full-range constants/bindings, compile-time overflow, default signed inference and rejected type/tensor/GPU capabilities. The [shared unsigned runtime harness](../../native/tests/UInt32RuntimeTests.cpp), [width specialization](../../native/cmake/UnsignedScalarType.cmake) and [compiled driver](../../native/cmake/VerifyUInt32.cmake) compare seven operators with independent wider unsigned arithmetic. Tests cover 25-by-25 boundary pairs and 10,000 deterministic full-range input pairs per operator, native-width division/remainder, exact CLI values, mixed task layouts, nested/cached workers, all six faulting operations, source locations, recovery, zero-live-allocation cleanup and static-error artifact preservation. A separate mixed-packet program returns each of the five supported scalar families as its root result, including UINT64_MAX and INT64_MIN together. Debug, optimized, ASan/UBSan and TSan variants run task cases with 1/2/4 workers; frontend checks also run independently under ASan/UBSan. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-uint64-scalars-2026-10-17.json) records the regression and numerical artifact comparison.

Shared int64/uint64 tensor literals and elementwise arithmetic are implemented below. Tensor worker handoff, reductions and matmul are qualified below; GPU integer arithmetic, float64 and explicit numeric conversions remain gated. Distributed execution still requires the agreed design review.

## Shared int64/uint64 CPU tensor literals and elementwise arithmetic

The synchronous CPU subset supports both `Tensor[int64,...]` and `Tensor[uint64,...]` through one ranked tensor lowering. The `int` and `uint` aliases keep their 64-bit meanings in tensor element context too. No source conversion, width promotion or signed/unsigned mixing is implicit.

```kalandra
fn shift(values: Tensor[int64,?], amount: int64) -> Tensor[int64,?]:
  values + amount

fn offsets() -> Tensor[int64,2]:
  tensor([9007199254740993,-9223372036854775808])

fn counters() -> Tensor[uint64,2]:
  tensor([9223372036854775808,18446744073709551615])
```

Literals retain exact eight-byte integer cells, including values beyond floating-point integer precision. Nested and explicit-shape literals, rank-zero tensors, fixed/dynamic extents and ordinary function returns use the existing tensor machinery. Binary `+`, `-`, `*`, `/` and `%` work on equal-shaped tensors or with a scalar of the same element type in either operand position. There is no broadcasting. Unary `+` preserves the value; unary `-` checks signed minimum, or requires zero for unsigned cells. Empty tensors perform no lane arithmetic, so an empty tensor divided by zero remains empty.

Each lane reuses the checked scalar lowering. Signed add/subtract/multiply and negation use signed i128 intermediates, check int64 bounds and narrow. Unsigned add/multiply use unsigned i128; subtraction checks operand order. Division/remainder use guarded native i64 instructions: zero divisors fault, INT64_MIN / -1 faults, and INT64_MIN % -1 returns zero. There is no wrapping or saturation. A late lane fault publishes no partial result, retains its source location and settles all invocation allocations.

Shape validation and allocation guards use the numeric description's eight-byte element size, including private native input descriptors. Both statically known and dynamic element counts above INT64_MAX / 8 are rejected before allocation or reading elements. Borrowed strided/offset views and aliased inputs remain immutable; returned allocation leases can outlive their invocation. These interfaces are internal compiler/runtime test surfaces, not a public foreign-memory ABI.

The [shared harness](../../native/tests/Integer64TensorRuntimeTests.cpp) and [compiled driver](../../native/cmake/VerifyInteger64Tensors.cmake) qualify both signednesses with independent 128-bit arithmetic references. Coverage includes full-range boundary/random pairs, scalar operand order, rank zero through three, all empty-axis positions, noncontiguous inputs, aliases, late faults, shape refinement/mismatch, negative and oversized dimensions, allocation rejection, retained results and cooperative cancellation. Debug, optimized, ASan/UBSan and TSan variants cover 25 generated programs per type. Shared frontend checks cover contextual literals, strict type rules and backend rejection of forged reduction/matmul IR. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-integer64-tensors-2026-10-17.json) records the full regression and float32 artifact comparison.

Shared int64/uint64 tensor worker captures/results and lifetime handling are qualified below. Their reductions and matmul are qualified below; Metal integer operations remain gated separately. Distributed execution still requires the agreed design discussion.

## Shared int64/uint64 tensor captures and results in CPU tasks

Native CPU workers can now capture and return both `Tensor[int64,...]` and `Tensor[uint64,...]`. The common continuation planner carries their existing numeric identity, and task layout keys/IDs distinguish each element type and rank. Signed and unsigned 64-bit descriptors have identical native sizes but distinct type identities; observing an outcome with the other type's identity fails before copying or retaining data. Existing int32/uint32/float32 layout identities remain unchanged. These are private process-local interfaces, not a serialized contract or public foreign-memory ABI.

```kalandra
fn shift(values: Tensor[int64,?], amount: int64) -> Tensor[int64,?]:
  values + amount

fn batch(values: Tensor[int64,?]) -> Tensor[int64,?] ! TaskError:
  try parallel:
    work = spawn shift(values, 1)
    first = try await work
    again = try await work
    first + (again - first)

fn main() -> int ! TaskError:
  values: Tensor[int64,2] = tensor([9007199254740993,-10])
  try parallel:
    adjusted = try await spawn batch(values)
    0
```

The same code structure works with uint64 context and its checked arithmetic rules. Worker capture/result copies retain the backing allocation rather than copying tensor cells. Nested groups and repeated awaits share immutable values; result ownership can outlive the producing task, worker pool and destination invocation. Arithmetic faults preserve their source location and publish no successful tensor payload. Recovery can reuse the unchanged parent tensor and launch a new group. Cancellation, deadlines and allocation/admission rejection reuse the existing settlement machinery; no new scheduler or allocator is introduced.

The [shared task driver](../../native/cmake/VerifyInteger64TensorTasks.cmake) specializes the same scenarios for signed and unsigned 64-bit tensors. The [existing tensor lifetime harness](../../native/tests/Int32TensorTaskTests.cpp) now checks both widths, exact returned cell bits/bytes, cached observation, cross-signedness layout rejection, retained values surviving owner destruction and zero live scheduler/allocation state. Tests cover rank zero through three, empty tensor arithmetic, nested tasks, shape faults, every faulting arithmetic operation, Arithmetic recovery with parent-data reuse, root admission rejection, cancellation/deadline signals and allocation-failure injection at positions 1 through 24.

A [mixed-packet fixture](../../native/tests/fixtures/task-integer64-tensor-mixed.k) captures all five implemented tensor families plus both 64-bit scalar families, using INT64_MIN and UINT64_MAX scalar values to detect truncation. Each tensor family is observed as the root result. It includes full-range signed/unsigned cells and integers beyond 2^53. Debug, optimized, ASan/UBSan and TSan variants run every handoff scenario with 1/2/4 workers; frontend task typing also runs independently under ASan/UBSan. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-integer64-tensor-tasks-2026-10-17.json) records the full native/Metal regression and unchanged float32 numerical artifacts. Host sanitizers do not instrument Metal shaders.

Shared checked int64/uint64 reductions are qualified below. Matmul is qualified below, with backend admission guards against mismatched element types. GPU integer arithmetic, further numeric formats and explicit conversions remain separate work. Discuss native distributed execution before implementation.

## Shared checked int64/uint64 CPU reduceSum

`reduceSum` now accepts both int64 and uint64 CPU tensors and returns the same scalar element type. It supports rank zero, fixed/dynamic shapes and empty tensors. Inputs are traversed in row-major order. Adjacent pairs are summed, an odd final element is carried unchanged, and the tree repeats until one value remains. Empty input returns an exact typed zero.

```kalandra
fn sum(values: Tensor[uint64,?]) -> uint64:
  reduceSum(values)

fn main() -> uint64 ! TaskError:
  values: Tensor[uint64,3] = tensor([9223372036854775808,9223372036854775807,0])
  try parallel:
    work = spawn sum(values)
    first = try await work
    again = try await work
    first + (again - first)
```

This returns 18446744073709551615 exactly. Signed input can likewise return INT64_MIN and INT64_MAX without floating conversion. Every tree addition must fit its source type: `[INT64_MAX, 1, -1]` faults at the first pair even though the mathematical total fits; `[INT64_MAX, -1, 1]` succeeds. Integer bounds and checked arithmetic apply in every build mode. There is no widened-total accumulator, wraparound or reassociation.

Both types reuse the existing tree and checked scalar addition lowering. Signed and unsigned 128-bit intermediates check each addition before narrowing to its 64-bit result; these are compiler intermediates, not new source types. Scratch storage contains eight-byte cells. Input and scratch element counts are checked against INT64_MAX / 8 before memory access or allocation. Zero-element shapes with a huge unused extent still return zero without reading input cells. Existing float32 and 32-bit integer reduction behavior remains unchanged.

Nested workers can capture the tensor and return the scalar; repeated awaits retain the same outcome. Arithmetic faults preserve the reduction source location and expose no successful scalar value. Recovery may reuse the unchanged parent tensor and launch another group. The shared invocation cleanup paths handle scratch allocation rejection and cooperative cancellation.

The [shared 32/64-bit runtime harness](../../native/tests/Int32ReductionRuntimeTests.cpp) compares exact results with independent wider adjacent-pair arithmetic. It covers rank zero through three, full-range signed/unsigned endpoints, values above 2^53, odd/multilevel trees, deterministic full-width inputs, first/later-level overflow, padded offset/strided views, empty axes and huge unused extents, invalid dimensions/byte counts, scratch allocation rejection, input immutability, cancellation and settled owners. The [shared compiled driver](../../native/cmake/VerifyInt32Reductions.cmake) adds exact CLI endpoints, nested/cached worker calls, empty worker results, source-located faults, Arithmetic recovery with parent-data reuse and allocation injection at positions 1 through 24 with 1/2/4 workers. Root bridge allocation failure and admitted-task faults are both checked for cleanup; successful native entries already reject live invocation allocations.

Debug, optimized, ASan/UBSan and TSan builds qualify both types. Frontend checks also run independently under ASan/UBSan and backend admission rejects mismatched reduction element types. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-integer64-reductions-2026-10-17.json) records the full native/Metal regression and unchanged float32 numerical artifacts. Host sanitizers do not instrument Metal shaders.

Shared checked int64/uint64 matrix multiplication is qualified below. GPU integer arithmetic and further numeric formats/conversions are separate work. Discuss native distributed execution before implementation.

## Shared checked int64/uint64 CPU matmul

`matmul` now accepts rank-two int64 and uint64 CPU tensors. Both operands and the result have the same concrete element type. Inner extents must match; fixed mismatches are diagnosed before code generation, and dynamic mismatches fault before memory access. There is no implicit conversion between integer widths, signedness or floating formats.

```kalandra
fn product(a: Tensor[uint64,?,?], b: Tensor[uint64,?,?]) -> Tensor[uint64,?,?]:
  matmul(a, b)

fn main() -> uint64 ! TaskError:
  a: Tensor[uint64,1,2] = tensor([[9223372036854775808,9223372036854775807]])
  b: Tensor[uint64,2,1] = tensor([[1],[1]])
  try parallel:
    work = spawn product(a, b)
    result = try await work
    reduceSum(result)
```

This returns 18446744073709551615 exactly. Signed matrices likewise preserve INT64_MIN, INT64_MAX and values beyond floating-point integer precision. Each product must fit its source integer type before entering the sum tree. Products are formed in ascending contraction-index order, then combined by the checked adjacent-pair tree used by `reduceSum`. Every tree addition must also fit. A later cancellation in the mathematical total cannot rescue an earlier product or pair overflow. There is no widened accumulator, wrapping or saturation.

The common integer kernel now selects cell and scratch types from the numeric element type. Int64 products and sums use signed i128 intermediates for bounds checks; uint64 uses unsigned i128. All possible pairs of 64-bit operands have exact products in these intermediate formats. Each checked result retains its 64-bit type. This extends the existing 32-bit kernel without changing its arithmetic or the float32 SIMD paths.

Output and scratch allocation checks use eight bytes per cell. Zero inner extents produce typed zero matrices. Zero output cells skip contraction work and contraction-sized scratch allocation; a huge unused inner extent is valid when both input descriptors are empty. Nonempty input descriptors still receive their own byte-size checks. Borrowed offset/strided and transposed views remain immutable. A fault in a later product or output cell exposes no partial matrix; owned results remain valid after invocation settlement.

Workers can capture both matrices, return the result tensor, and retain it through nested groups and cached awaits. Arithmetic faults retain the matmul source location; recovery can reuse the original parent matrices and start a new group. Existing ownership, cancellation, deadlines and allocation/admission rejection paths are reused.

The [shared 32/64-bit runtime harness](../../native/tests/Int32MatmulRuntimeTests.cpp) compares exact cells with independent wider products and an adjacent-tree reference, checking bounds at each operation. Coverage includes endpoints, above-2^53 values, UINT64_MAX squared, products around 2^32, signed-minimum negation through multiplication, odd/multilevel trees, deterministic small/full-width inputs, aliases, padded/transposed views, late-cell faults, empty shapes, invalid dimensions and exact byte limits, scratch/output allocation rejection, retained result leases and cooperative cancellation.

The [shared compiled driver](../../native/cmake/VerifyInt32Matmul.cmake) adds fixed-shape matrices, full-range CLI endpoints, nested/cached worker results, product/pair/later-level overflow, Arithmetic recovery with parent-data reuse, source-located failures, empty worker outputs, static-error artifact preservation and allocation injection at positions 1 through 24 with 1/2/4 workers. The [existing lifetime harness](../../native/tests/Int32TensorTaskTests.cpp) checks results surviving worker/core/invocation destruction, layout identity, and zero live scheduler/allocation state.

Debug, optimized, ASan/UBSan and TSan variants qualify both types. Frontend checks also run independently under ASan/UBSan, and backend admission rejects mismatched matmul element types. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-integer64-matmul-2026-10-17.json) records the native/Metal regression and unchanged float32 numerical artifacts. Host sanitizers do not instrument Metal shaders. GPU integer arithmetic remains gated.

Next: CPU float64 literals and scalar arithmetic, so the accepted `float` default has an executable implementation. Further tensor formats and explicit conversions remain separate work. Discuss native distributed execution before implementation.

### CPU binary64 arithmetic lowering proof

The shared [strict floating lowering](../../native/lib/StrictFloat.cpp) now has a CPU-only binary64 specialization for add, subtract, multiply, divide and sign-bit negation. Binary32 and binary64 share exponent alignment, sticky-bit shifts, normalization and nearest/ties-to-even rounding. Binary64 uses i128 intermediates for its 53-bit significand products and division numerator; binary32 retains its existing i64 sequence. This is a correctness implementation, not a hardware-double performance claim.

Each arithmetic result rounds separately, including multiply followed by addition. Subnormal values are preserved, division by zero follows IEEE behavior, and finite arithmetic may overflow to infinity. Arithmetic NaNs are canonicalized internally; unary negation flips only the sign bit, preserving signed zero and NaN payload bits. The generated arithmetic uses integer operations, so host rounding, flush-to-zero and default-NaN settings do not change these results.

The [CPU-only proof tool](../../native/metal-proof/Strict.cpp) accepts mixed scalar binary32/binary64 modules. It rejects GPU/SPIR-V operations, unsupported formats, vectors, comparisons, remainder and conversions. The established binary32/Metal entry point still rejects binary64. [Compiled conformance](../../native/metal-proof/Strict64Cpu.cpp) compares boundary and full-range deterministic inputs against LLVM APFloat in debug, optimized and ASan/UBSan builds. It installs all four ARM rounding modes with flush-to-zero/default-NaN both off and on, checks floating exception flags, and restores the original host state. Artifact checks verify integer-only arithmetic and transactional rejection of unsupported inputs. These CPU tests are currently built with the optional Metal proof configuration, but do not submit GPU work.

[Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-strict-float64-2026-10-17.json) records 467,411 operand pairs, 22,435,728 exact result comparisons per build variant, 30 focused regression checks and preservation of all 640 tracked binary32 artifacts.

This first qualified the internal arithmetic building block. Source integration is now implemented below; float64 tensors and GPU arithmetic remain separate work. Distributed execution still requires the agreed design review.

### CPU float64 scalar source and worker integration

The native CPU subset now accepts `float64` scalars and the platform-independent `float` alias. Decimal/exponent literals without a numeric context default to float64; whole-number literals still default to int64. A literal-only arithmetic expression containing a decimal/exponent literal uses float64 when unconstrained, regardless of operand order. A declared parameter, result or binding can provide a different implemented numeric context. Already typed values never implicitly change width or numeric family.

```kalandra
fn adjust(value: float) -> float:
  value + 0.0000000000000002

fn main() -> float ! TaskError:
  try parallel:
    work = spawn adjust(1.0)
    try await work
```

This prints `1.0000000000000002`. Literal text is parsed directly into binary64 with nearest/ties-to-even rounding, including gradual underflow; it is never parsed through float32. Finite literals that overflow binary64 are rejected with S015 before publishing output artifacts. Tiny literals may round to zero. Unconstrained `-0.0` preserves negative zero; plain `-0` remains an integer literal.

Scalar `+`, `-`, `*`, `/` and unary signs use the [qualified strict arithmetic engine](#cpu-binary64-arithmetic-lowering-proof). The compiler lowers each binary64 operation through integer MLIR before LLVM conversion, preserving separately rounded products/sums and subnormal behavior under all tested host floating environments. This implementation prioritizes correctness; hardware-double performance is not claimed. IEEE infinity and NaN results are ordinary float values, including division by zero; they do not become Arithmetic faults. Integer arithmetic faults remain checked and can be recovered while preserving captured float64 values. Float64 tensors, reductions and matrix multiplication are qualified below. Floating remainder, implicit conversions and float64 in Metal source modules remain explicitly unsupported.

Dedicated native entries return LLVM `double` and print 17 significant decimal digits. Existing float32/integer entry symbols retain their types. Worker captures, continuation slots and result wrappers use binary64 payloads and a distinct private layout identity, even when other numeric layouts share size/alignment. Binary64 worker arithmetic uses the same strict MLIR kernels as synchronous functions. Nested calls, aliases and repeated awaits preserve exact bits; result observation rejects float32/int64/uint64 layout impostors before publishing data. The scheduler and allocation runtime are reused without numeric-specific changes.

[Frontend checks](../../native/tests/Float64Tests.cpp) cover direct literal rounding and ties, maximum finite/minimum normal/subnormal values, aliases/defaulting, unsupported mixing and tensor/GPU rejection, including forged typed tensor IR. The [compiled scalar driver](../../native/cmake/VerifyFloat64scalars.cmake) and [runtime harness](../../native/tests/Float64RuntimeTests.cpp) share the [APFloat oracle/corpus](../../native/metal-proof/Float64Oracle.h) with the arithmetic proof. They verify exact source-generated results across eight ARM floating environments, source/native entries, CLI round trips and cancellation gating. The [worker driver](../../native/cmake/VerifyFloat64tasks.cmake) and [lifetime harness](../../native/tests/Float64TaskTests.cpp) cover all six implemented scalar families in capture packets, every root result family, nested/cached calls, signed zero, subnormals, infinity, NaN, separate rounding, recovery with owned parent tensors, layout rejection, cancellation/deadlines/admission, and allocation injection at positions 1 through 24 with 1/2/4 workers. Debug, optimized, ASan/UBSan and separate TSan builds qualify compiled execution; frontend checks also run under ASan/UBSan.

[Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-float64-scalars-2026-10-17.json) records all 311 native/Metal checks passing, 89,742,912 exact source-result comparisons across four build variants and eight floating environments, 432 worker lifetime process runs, and preservation of all 640 tracked float32 artifacts. The ordinary compiler also builds and runs float64 workers with both Metal options disabled.

CPU float64 tensor literals, elementwise arithmetic, worker handoff, reductions and matrix multiplication are qualified below, reusing width-aware storage and the strict scalar engine. Explicit conversions remain separate work. Discuss native distributed execution before implementation.

### CPU float64 tensor literals and elementwise arithmetic

Synchronous CPU functions now accept `Tensor[float64, ...]` and `Tensor[float, ...]`. Rank-zero, vector, matrix and higher-rank literals preserve their binary64 elements. Nested arrays or flat data with an explicit constant shape use the existing checked tensor constructor, including empty vectors and empty matrices. Finite-literal overflow remains a compile-time S015 error; tiny values round directly to binary64, and negative zero is preserved.

```kalandra
fn shifted(values: Tensor[float, ?]) -> Tensor[float, ?]:
  values + 0.25

fn example() -> Tensor[float, 3]:
  values: Tensor[float, 3] = tensor([1.0000000000000002, -0.0, 4.9406564584124654e-324])
  shifted(values)
```

Elementwise `+`, `-`, `*`, `/`, unary signs, and either scalar operand order use the existing shape-checked tensor loops. Binary64 operations inside those loops pass through the same strict integer lowering as scalar arithmetic. Every elementary operation rounds separately; `a*b+a` retains the rounded product. Division by zero, infinities and NaNs follow the scalar IEEE behavior. Negation flips exactly the sign bit; unary plus preserves every element bit. There is no implicit float32/float64 or integer/float conversion and no floating remainder.

The existing eight-byte storage, allocation-size checks, strided memref access and invocation ownership machinery serve this type without a new runtime or numerical kernel implementation. Borrowed inputs remain immutable, including aliased and transposed views. Returned backing can be retained across invocation settlement. Shape and allocation failures publish no partial result; generated loops cooperate with cancellation.

CPU worker captures/results are now supported as described below; the temporary S054 module restriction has been removed. Float64 `reduceSum` and `matmul` are now qualified below; backend admission still rejects mismatched element types. Float64 remains unsupported in Metal source modules.

[Frontend checks](../../native/tests/Float64TensorChecks.h) cover literal ranks, aliases, scalar lifting, strict numeric identities, exact eight-byte size boundaries and unsupported operations/targets. The [compiled driver](../../native/cmake/VerifyFloat64Tensors.cmake) and [runtime harness](../../native/tests/Float64TensorRuntimeTests.cpp) reuse the [APFloat corpus](../../native/metal-proof/Float64Oracle.h) across arithmetic, unary signs and separately rounded composition. Ranked tests exercise all 467,411 operand pairs; scalar-lift tests choose one scalar per batch and calculate the corresponding lane reference. Rank zero uses 4,096 samples spanning the corpus. Eight ARM floating environments cover all rounding modes with flush-to-zero/default-NaN disabled and enabled, including literal and refinement tests. Result bits are checked both before and after invocation settlement. Additional probes cover empty axes, padded/displaced/transposed/aliased inputs, dynamic shape mismatch, exact/overflowing byte bounds, allocation injection and poll-triggered cancellation. Debug, optimized, ASan/UBSan and separate TSan builds qualify generated execution; frontend checks also run independently under ASan/UBSan.

[Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-float64-tensors-2026-10-17.json) records 37 focused frontend, scalar/worker and strict Metal regression checks passing, plus the final four tensor variants with strengthened non-square view probes. It records 628,872 tensor invocations, exact lane checks before/after settlement, and preservation of all 640 tracked float32 artifacts. The backend, strict arithmetic engine, runtime and common continuation planner are unchanged.

Float64 reductions and matrix multiplication are qualified below. Explicit numeric conversions and GPU numeric formats remain separate work. Discuss native distributed execution before implementation.

### Float64 tensor captures and results in CPU tasks

CPU workers accept and return `Tensor[float64, ...]` (including the `float` alias) through the existing continuation planner and retained tensor storage. Each rank has a distinct float64 layout identity. Float32, int64 and uint64 descriptors cannot be observed as float64 results even when their descriptor size, alignment and copy callback match.

```kalandra
fn shifted(values: Tensor[float, ?], amount: float) -> Tensor[float, ?]:
  values + amount

fn example() -> Tensor[float, ?] ! TaskError:
  values: Tensor[float, 2] = tensor([1.0000000000000002, -0.0])
  try parallel:
    work = spawn shifted(values, 0.25)
    first = try await work
    again = try await work
    again
```

Captures retain their backing while a worker uses them. Nested calls and repeated awaits preserve ownership, and explicitly retained native results can outlive their scheduler and receiving invocation. Arithmetic still uses the shared strict binary64 engine; passing a tensor between workers does not convert its elements. Floating division by zero produces IEEE results rather than an Arithmetic task fault. Shape/allocation failures and checked integer faults in sibling work use the existing TaskError supervision and recovery paths.

The [worker driver](../../native/cmake/VerifyFloat64TensorTasks.cmake) and [shared retained-result harness](../../native/tests/Int32TensorTaskTests.cpp) check ranks zero through three, empty matrices, mixed captures from all six supported tensor families, float64 scalar captures, exact signed-zero/subnormal/precision-boundary/infinity/NaN results, nested and cached awaits, transactional layout rejection and results surviving their owners. They also exercise shape faults, recovery after an integer sibling fault, allocation injection, cancellation, deadlines and admission rejection with one, two and four workers. Debug, optimized, ASan/UBSan and TSan builds are qualified. The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-float64-tensor-tasks-2026-10-17.json) records 57 focused checks passing across the regression run and final worker rerun, including 444 float64 tensor worker process runs. All 640 tracked float32 numerical artifacts remain unchanged. The strict engine, CPU tensor backend, continuation planner, storage and runtime are unchanged.

Float64 reductions and matrix multiplication are qualified below. Explicit numeric conversions and GPU execution remain separate milestones. Discuss native distributed execution before implementation.

### CPU float64 reduceSum

`reduceSum` accepts a CPU `Tensor[float64, ...]` or `Tensor[float, ...]` and returns the same scalar element type. It uses the existing row-major balanced-adjacent reduction tree: add neighboring pairs and carry an odd tail unchanged to the next level. Each addition rounds separately to binary64, nearest with ties to even. The compiler does not reassociate the tree or narrow intermediate values to float32.

```kalandra
fn sum(values: Tensor[float, ?]) -> float:
  reduceSum(values)

fn example() -> float ! TaskError:
  values: Tensor[float, 3] = tensor([1.0000000000000002, 2, 3])
  try parallel:
    try await spawn sum(values)
```

A rank-zero tensor or one-element input returns its element bits unchanged, including negative zero and NaN payloads. Any zero extent produces positive zero without reading input elements. Actual additions canonicalize NaNs according to the scalar arithmetic contract; overflow produces infinity rather than an Arithmetic fault. Shape/byte-bound checks, scratch allocation failures and cooperative cancellation use the existing native execution paths. Scratch storage settles on success and failure. The input remains immutable, including padded, displaced and transposed native views.

Ordinary functions, direct `spawn reduceSum(values)`, nested workers, aliased handles and cached awaits share the same reduction lowering. Typed float64 scalar result layouts preserve exact results. Handlers can reuse owned tensor data after a sibling task fails and start a fresh parallel group.

The [compiled driver](../../native/cmake/VerifyFloat64Reductions.cmake) and [runtime harness](../../native/tests/Float64ReductionRuntimeTests.cpp) compare result bits against an independent APFloat adjacent-pair tree. Tests cover ranks zero through three, empty axes, singleton payload preservation, sampled boundary/random operand pairs, odd lengths and power-of-two neighbors, longer trees, row-major strided access, exact eight-byte bounds, injected allocation failures and cancellation. Eight ARM floating environments check independence from host rounding/flush-to-zero/default-NaN controls and absence of floating exception flags. Worker tests reuse the existing scalar result harness for nested/cached calls, recovery, scheduler and invocation allocation injection, cancellation, deadlines and admission rejection with one, two and four workers. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-float64-reductions-2026-10-17.json) records 66 focused checks passing across the regression run and updated capability-test rerun, including 791,572 ranked reduction invocations and 636 worker process runs across debug, optimized, ASan/UBSan and TSan builds. All 640 tracked float32 numerical artifacts remain unchanged. The strict arithmetic engine, task backend, continuation planner, tensor storage and runtime are unchanged.

Float64 matrix multiplication is qualified below. Explicit conversions and GPU formats remain separate work; discuss native distributed execution before implementation.

### CPU float64 matmul

`matmul` accepts two rank-two CPU float64 tensors (including the `float` alias) and returns a float64 matrix. Each output cell uses separately rounded binary64 products followed by the same balanced-adjacent sum tree as `reduceSum`; an odd tail moves to the next level unchanged. Products and sums use the existing strict arithmetic engine. There is no fused multiply-add, reassociation, implicit conversion or float32 intermediate.

```kalandra
fn product(a: Tensor[float, ?, ?], b: Tensor[float, ?, ?]) -> Tensor[float, ?, ?]:
  matmul(a, b)

fn example() -> Tensor[float, ?, ?] ! TaskError:
  a: Tensor[float, 2, 2] = tensor([[1, 2], [3, 4]])
  b: Tensor[float, 2, 2] = tensor([[2, 0], [0, 2]])
  try parallel:
    try await spawn product(a, b)
```

A zero contraction length produces positive-zero output cells without reading input elements or performing arithmetic. Zero rows or columns preserve the output shape and skip contraction work and contraction-sized scratch. Inner dimensions and allocation byte counts are checked; failures publish no successful tensor. Arithmetic NaNs and infinities remain ordinary IEEE values. Returned tensors use the existing retained backing, including across nested workers, cached awaits and task recovery. Inputs remain immutable, including aliased and noncontiguous native views.

The existing scalar matrix-multiplication path now serves both checked integers and float64. It selects strict floating multiplication for float64 and uses the already type-aware reduction scratch/tree. The float32 tiled SIMD path is unchanged. This is the float64 correctness baseline; float64 tiling/vectorization remains a later optimization under the same numerical contract.

The [compiled driver](../../native/cmake/VerifyFloat64Matmul.cmake) and [runtime harness](../../native/tests/Float64MatmulRuntimeTests.cpp) compare exact output bits with independently computed APFloat products and adjacent-pair sums. Probes cover sampled operand boundaries, random matrices, odd and long contractions, separately rounded/FMA-sensitive results, zero inner/output dimensions, padded/displaced/transposed and aliased views, eight ARM floating environments, immutable inputs, retained results, malformed metadata, allocation injection and cancellation. Worker probes use the shared tensor lifetime/layout harness for direct builtin and ordinary calls, nested/cached awaits, empty results, zero-length contractions, recovery after an integer sibling fault, allocation failures, cancellation, deadlines and admission rejection with one, two and four workers. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-float64-matmul-2026-10-17.json) records 70 focused checks passing across debug, optimized, ASan/UBSan and TSan builds, including 279,384 matmul invocations, 693,888 exact cell checks before/after settlement and 372 worker process runs. All 640 tracked float32 numerical artifacts remain unchanged, as do the strict engine, task backend, continuation planner, tensor storage and runtime.

Explicit numeric conversions are next. Additional numeric types, float64 performance work and GPU formats remain separate milestones. Discuss native distributed execution before implementation.

### Explicit CPU scalar numeric conversions

The initial six-type qualification covered all 36 pairs among `int32`, `uint32`, `int64`, `uint64`, `float32` and `float64` for explicit conversion through ordinary type-name calls. `int`, `uint` and `float` retain their int64/uint64/float64 aliases. There is no implicit promotion, wrapping or saturation.

```kalandra
fn ratio(count: int64) -> float:
  float(count) / 2.0

fn truncate(value: float64) -> int32:
  int32(value)

fn approximate(value: uint64) -> float32:
  float32(value)
```

Integer-to-integer conversion preserves the mathematical value or raises an ArithmeticFault if it cannot fit. Integer-to-float conversion explicitly authorizes precision loss and rounds to nearest, ties to even. Float32-to-float64 preserves finite values exactly. Float64-to-float32 uses ties-to-even rounding, preserves signed underflow to zero, and rejects finite overflow to infinity. Infinity remains infinity and NaNs remain NaNs when changing floating width; changed-width NaNs are canonicalized. Identity conversions preserve the original bits, including signed zero and NaN payloads.

Float-to-integer conversion truncates toward zero, then checks the destination bounds. NaN and infinity fail. A negative fraction between -1 and 0 truncates to zero and therefore converts to an unsigned integer; a negative integral result does not. For example, `uint(-0.9)` is zero and `uint(-1.0)` is invalid. Already typed operands keep their source type while converting. Arguments otherwise use ordinary literal inference; a whole-number literal beyond int64's range requires an explicitly typed uint64 binding before conversion.

Known-invalid scalar conversions, including through constant bindings and arithmetic, produce S055 before executable generation. Invalid runtime conversions raise an ArithmeticFault at the conversion call. An ordinary execution unit stops; supervised workers expose TaskError.Fault with FaultKind.Arithmetic. Handlers can reuse owned input tensors and start fresh parallel work. Direct `spawn float32(value)` calls and conversions inside named worker functions use the existing continuation/kernel machinery, typed result layouts and cached-await paths.

The new lowering implements conversions using bounded integer bit operations and checked ranges. It does not rely on hardware floating casts, host rounding/flush-to-zero/default-NaN settings, or undefined/out-of-range float-to-integer instructions. The existing arithmetic engine, runtime and tensor storage remain shared; no conversion runtime library is introduced. Numeric type names are reserved as function names, and conversion calls require one ordinary CPU numeric argument. Tensor conversions are described below; GPU conversions and the unimplemented narrow numeric types remain gated.

The [frontend tests](../../native/tests/ConversionTests.cpp), [compiled driver](../../native/cmake/VerifyConversions.cmake) and [runtime oracle harness](../../native/tests/ConversionRuntimeTests.cpp) cover all 36 pairs, endpoints and full-range random inputs, halfway ties, subnormal/overflow boundaries, truncation around signed/unsigned limits, infinity, NaN and signed zero. Results are compared with independent integer-range calculations and LLVM APFloat/APSInt under eight ARM floating environments, with no floating exception flags. The [worker harness](../../native/tests/ConversionTaskTests.cpp) checks all six result types, ordinary and direct builtin calls, repeated awaits, Arithmetic fault locations, recovery with owned parent data, scheduler/invocation allocation injection, cancellation, deadlines, admission rejection and settled owners with one, two and four workers. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-numeric-conversions-2026-10-17.json) records 107 focused checks passing across debug, optimized, ASan/UBSan and TSan builds: 15,585,408 conversion invocations (including 4,925,120 checked Arithmetic faults) and 936 worker processes. All 640 tracked float32 numerical artifacts remain unchanged, along with the existing strict arithmetic engine, task backend, storage, runtime and Metal implementation.

### Explicit CPU tensor element conversions

The [tensor conversion contract](tensors.md#explicit-cpu-element-conversions) extends type-name calls to tensors of all ten implemented CPU numeric types. Each lane reuses the scalar conversion lowering without changing its numerical rules. Shapes and immutable input values are preserved. Invalid elements raise ArithmeticFault at the cast; failed allocations, invalid metadata and cancellation cannot publish a partial output. Tensor literal conversions are checked at runtime rather than constant-folded.

The [compiled driver](../../native/cmake/VerifyTensorConversions.cmake) and [tensor harness](../../native/tests/TensorConversionRuntimeTests.cpp) share the independent [conversion oracle and scalar corpus](../../native/tests/ConversionOracle.h). The initial six-type qualification covered all 36 pairs on rank-two strided views and representative rank-zero, rank-one and rank-three conversions, with exact lane bits under eight ARM floating environments. They exercise invalid values at the first, middle and last positions, empty axes, input/destination byte bounds, allocation failure, retained results and cooperative cancellation. Worker probes cover every source/destination pair, direct casts, nested workers, cached awaits, recovery with owned input tensors, typed layout rejection, admission, cancellation, deadlines and allocation failures with one, two and four workers.

The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-tensor-conversions-2026-10-17.json) records 111 focused checks passing across debug, optimized, ASan/UBSan and TSan builds: 293,456 tensor-conversion invocations (including metadata/allocation/Arithmetic fault probes), 22,107,008 exact lane checks before/after settlement and 1,200 worker harness runs. The scalar regression repeats 15,585,408 conversion invocations. All 640 tracked float32 numerical artifacts, the scalar conversion algorithm, strict engine, continuation planner, task backend, storage/runtime and Metal implementation remain unchanged. This is focused qualification rather than the entire 337-test suite.

Additional source types, GPU formats and broader language support remain separate milestones. Discuss native distributed execution before implementation.

Current [CPU tensor conversion coverage](narrow-tensor-conversions.md) extends this contract to all 100 pairs among the ten implemented numeric types, with renewed exact-bit, worker and sanitizer qualification.

## CPU int8 and uint8 scalar foundation

[Checked byte scalars](int8-scalars.md) now implement signed/unsigned 8-bit literal typing, arithmetic, comparisons, explicit scalar conversions, local worker captures/results and CLI display. [Checked 16-bit scalars](int16-scalars.md) complete the ten accepted scalar types, sharing the same arithmetic and conversion machinery. [Narrow arrays and scalar/array record fields](narrow-arrays.md) now preserve exact 8/16-bit storage through local workers; [narrow tensor construction and checked elementwise operations](narrow-tensors.md) now share the same integer lowering. [Checked narrow CPU reductions](narrow-reductions.md) now complete CPU reduction coverage for all ten types. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten types. GPU execution remains separately gated.
