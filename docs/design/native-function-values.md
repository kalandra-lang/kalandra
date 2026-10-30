# Named CPU function values

Current CPU extension: [proven coordinating function values](native-resumable-callables.md) now use checked continuation dispatch, managed environments and the shared worker scheduler. This supersedes the synchronous-only coordination restriction in this earlier implementation record; recursive coordinating calls now use bounded CPU continuations; unknown targets and GPU callables remain gated.

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Implemented native subset — 2026-10-19. Named synchronous CPU functions, including expected-error callables, can now be assigned to bindings, passed as parameters, selected at runtime and returned as values. This implements the first executable part of the accepted [function-value design](function-values.md); [managed expression closures](native-closures.md) extend this subset with returned captured functions.

## Assign, pass, return and call

```kalandra
fn double(value: int) -> int:
  value * 2

fn triple(value: int) -> int:
  value * 3

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn choose(flag: Bool) -> fn(int) -> int:
  if flag: double
  else: triple

export fn main() -> int:
  transform = choose(false)
  alias: fn(int) -> int = transform
  items = map([1, 2, 3], alias)
  apply(4, choose(true)) + items[2]
```

The result is `17`: `choose(false)` returns `triple`, the mapped array is `[3, 6, 9]`, and the last call adds `double(4)`. The [complete fixture](fixtures/function-values.k) can be compiled with the native compiler.

A binding copies a function reference without invoking it or consuming the original binding. `fn(int) -> int` is a structural signature; different named functions with that signature are interchangeable. Numeric widths and nominal record/enum declarations must still match exactly. A declaration with the same fields as another record is a different parameter type.

Function-valued parameters and results may themselves contain function signatures. Call chains such as `factory()(false)(14)` evaluate each callee before its arguments. Ordinary arguments evaluate once, left to right. Existing pipelines work with a function-valued binding: `10 -> combine(3)` means `combine(10, 3)` when `combine` is that binding. Function-value equality and reflection are not introduced.

## Maps and local workers

Sequential `map(values, transform)` accepts a compatible function-valued binding, parameter or expression. It keeps the existing one-element callback contract and input order. An inline map callback may also capture a completed function reference and invoke it. Callback creation is evaluated even for an empty input; the callback body is not invoked for empty inputs.

A local CPU worker can create and call function references internally, including expected-error calls and completed-result maps. A coordinating function can hold an ordinary function reference across its own task suspension and invoke it after awaiting data. [Capture-free named function arguments](native-worker-functions.md) can now cross to statically named CPU workers. [CPU data-capturing closure arguments](native-worker-closures.md) are also supported. [Direct nested function captures](native-worker-nested-closures.md) are also supported. Function-containing aggregate captures and unproven arguments, unproven function-valued worker results, spawned-map function captures and indirect spawn remain unsupported. The [local-worker transport contract](worker-function-transport.md) defines the planned ownership and implementation sequence.

Ordinary immutable records returned by these calls keep their existing managed field ownership. Record values with String and array fields can still be returned from a worker and observed repeatedly; the function reference itself is not part of that transported data.

## Collection parameters and results

A function value can accept or return an immutable collection directly:

```kalandra
fn keep(values: [int]) -> [int]:
  values

fn doubled(values: [int]) -> [int]:
  map(values, fn(value): value * 2)

fn choose(flag: Bool) -> fn([int]) -> [int]:
  if flag: doubled
  else: keep

fn apply(values: [int], transform: fn([int]) -> [int]) -> [int]:
  transform(values)

export fn main() -> int:
  transform = choose(true)
  values = apply([10, 11], transform)
  values[0] + values[1]
```

The [complete fixture](fixtures/function-collections.k) returns `42`. Numeric tensors use ordinary signatures such as `fn(Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]`. Rank, element type, fixed extents and the distinction between an array and a tensor remain part of the signature. Rank-zero tensors such as `Tensor[int]` use the same callable path. Function signatures must match exactly; this increment does not add variance or automatic numeric conversions.

An array length is a value fact, not part of the `[T]` signature. Inferred array-producing functions can therefore be assigned to an annotated callable and selected together even when they produce different lengths. A dynamic parameter extent accepts different concrete input sizes. A fixed extent is checked statically when known and guarded at runtime otherwise. A dynamic result extent has a fresh identity for each invocation: two calls through the same function value do not promise equally shaped results. Later operations still guard their shape requirements. The compiler does not infer a relationship between parameter and result extents from a callable signature alone.

Bool, String and record arrays preserve the existing descriptor representation and managed child storage. A named local worker may create a function reference, invoke it on collections and return collection data through the ordinary retained-result path, including repeated awaits. Transporting a proven function reference as a direct argument to a statically named CPU worker is supported; function-valued results and containing worker values remain staged. Inline map callbacks can call a captured function on a collection and produce a supported scalar element; arrays of arrays and tensor-valued map elements are not introduced.

The pure-MLIR numeric indirect-call path uses private checked descriptor adapters. It copies logical elements into managed backing and reconstructs tensors in the callee and caller. This preserves immutable values, borrowed/strided input ordering, empty shapes and checked failures, but it adds allocations and copies compared with a direct call. Shared-storage adapters and copy elimination are tracked in the [optimization roadmap](../roadmap.md#optimization); this implementation makes no zero-copy or performance claim. Ordinary statically named collection calls retain their existing lowering. Expected-error entries use the descriptor-valued LLVM path described below; surrounding operations and kernel bridges can still allocate or copy.

## Expected-error signatures

A callable's declared error set is part of its type:

```kalandra
error InputError:
  Invalid:
    value: int

fn checked(value: int) -> int ! InputError:
  if value == 0:
    fail InputError.Invalid {value: 42}
  value * 2

fn apply(value: int, transform: fn(int) -> int ! InputError) -> int ! InputError:
  try transform(value)

export fn main() -> int:
  transform = checked
  apply(0, transform) catch fn(error):
    match error:
      InputError.Invalid {value}: value
```

The [complete fixture](fixtures/fallible-functions.k) returns `42`. Binding `checked` creates a function value; invoking it produces a result whose error must be handled, propagated with `try`, or returned through a compatible channel. Numeric widths, nominal declarations and canonical error sets must match exactly. An infallible signature is a different type from one declaring an error, even when a particular implementation always succeeds. Aliases, repeated members and reordered unions resolve to the same canonical set.

Expected-error results can contain every supported completed callable result, including scalars, Strings, records, arrays, fixed/dynamic tensors and rank-zero tensors. Owned error fields keep their existing managed lifetimes. A result can be handled repeatedly without reevaluating its call; worker-produced errors preserve their fields through cached awaits and scheduler settlement. Shape refinements check successful collection values and forward an error without reading its absent tensor payload.

### Factories and returned functions

Group the returned function type before adding the factory's own error channel:

```kalandra
fn make(flag: Bool) -> (fn(int) -> int) ! BuildError
fn make(flag: Bool) -> fn(int) -> int ! InputError
fn make(flag: Bool) -> (fn(int) -> int ! InputError) ! BuildError
```

These are signature illustrations with separately declared error families. The first factory may return `BuildError`; its returned function is infallible. The second factory returns a function whose invocation may return `InputError`. The third has both channels. Function values can represent these factories, and `try`/`catch` can extract or replace their returned functions. Calling a pending factory result directly is rejected.

Callees evaluate before arguments, and arguments evaluate once in source order. If obtaining a callee propagates an expected error, its arguments are not evaluated. Checked arithmetic/allocation faults and cancellation still use the invocation context; a worker exposes such faults through its existing TaskError boundary. Returning an application error does not poison that context.

Maps continue to require completed element results. A bound infallible wrapper that recovers its own expected errors can be a callback. Mapping a fallible callable does not introduce an implicit aggregate-error policy. Static callbacks containing control-flow IR retain the current direct-map lowering restriction; the bound function-value route is qualified.

### Synchronous execution

Expected-error callables run on the caller's thread. Their entries reuse the existing continuation plan's error branches with a private outcome slot and the current invocation context. A checked temporary frame holds local values and is released on success, expected error, checked fault or cancellation. These entries do not submit tasks, enter scheduler waits or run a nested scheduler. Collection arguments/results use descriptors and keep their existing context ownership.

The compiler distinguishes synchronous error control flow from task/device coordination. Named functions that recover errors or match variants can now be function values too. A target that performs task/device work, including through a helper that catches its failures, remains rejected. [Recursive synchronous entries](native-recursive-callables.md) now support self/mutual calls and references with checked call admission. Frame allocation and direct-call specialization are tracked optimization work, not source-level allocation responsibilities.

## Supported signatures and remaining gaps

Callable signatures accept and return all ten CPU numeric scalar types, Bool, String, completed records/enums, numeric arrays/tensors, Bool/String/record arrays, [arrays of functions](native-function-arrays.md) and other supported function values. Completed TaskError values can be ordinary data, and TaskError can also be an invocation channel, alone or in an application-error union. Records may contain the already supported collection fields and [function-valued fields](native-callable-fields.md). The latter also work in enum and expected-error payloads within one invocation; function storage inside those containing types remains rejected at worker boundaries. Parameter counts have no special small cap: the compiler's ordinary parsing and complexity budgets apply. Qualification includes a ten-argument call mixing six numeric types, Bool, String, a record and another function.

The following remain implementation gaps under the accepted function-value design:

- Callable targets requiring task/device coordination. [Recursive synchronous calls](native-recursive-callables.md), expected-error calls and factories are implemented.
- Fallible map aggregation. [Direct synchronous control-flow callbacks](native-control-maps.md) and bound completed-result wrappers are supported.
- Captured/unproven worker function arguments, unproven function-valued worker results, containing-value transport, indirect spawn, GPU callbacks and remote function contracts.

These are staged implementation restrictions, not additional language design limits. Distributed contracts still require their separate design discussion; public C/C++ callback integration remains on its separate roadmap.

## Native lowering and checked execution

Typed IR distinguishes a named function reference from an invocation through a function value, including the invocation’s expected-error channel. The latter stores the callee value and checked argument types rather than choosing a single declaration during type checking. Completed-result calls use a typed MLIR indirect call; expected-error calls use the synchronous LLVM outcome entry described above; the current private representation is a code/environment pair. Named references need no environment allocation; captured anonymous functions use managed environments as described in [native closures](native-closures.md).

After LLVM translation, pure function addresses are redirected to checked entries and indirect calls receive the current invocation context. Synchronous control-flow entries are emitted with that context explicitly. The same failure polling, cancellation checks and cleanup rules used by direct calls therefore apply. A fault while computing the callee stops evaluation before its arguments; a fault during invocation stops subsequent result use. Original source locations are preserved. This is compiler/runtime plumbing, not a public native callback ABI or a serialization format.

Backend validation checks function signatures, producer operations, references, argument counts/types, compatible branch results, binding reads, returned function types and execution boundaries. Recursive signature comparison and validation have depth/work budgets. Invalid typed IR is rejected before lowering.

## Qualification

### Initial named subset

Focused frontend checks cover named aliases, higher-order parameters/results, call chains, runtime selection, maps, imported functions, nominal identity, unsupported captures/transport and malformed typed IR. Execution checks cover all ten numeric types, mixed argument passing, Bool/String/record/enum values, nested signatures, worker-local calls, cached worker data, checked faults, evaluation order, allocation failure and preservation of rejected output artifacts.

All 143 selected checks passed on Apple M2 Max, including the existing compiler, map, module, task and expected-error regressions. The new execution suites run in debug, optimized, ASan/UBSan and separate ThreadSanitizer variants, compiling 124 programs. Private runtime probes verify checked context forwarding, original fault locations, pre-cancelled/failed contexts and aggregate ownership settlement. Floating-point identity maps reuse the existing raw-bit/strided-array harness, with 768 invocations and 1,053,184 exact lane comparisons. CLI allocation-failure sweeps run 384 processes, alongside 100 aggregate-context allocation probes. The [qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-function-values-2026-10-19.json) records the scope, tests and frozen source hashes; these checks do not qualify escaping closures, function transport, a public ABI or other platforms.

### Collection signature extension

All 171 selected checks passed on Apple M2 Max: three focused frontend/debug checks followed by 168 remaining regression and sanitizer checks. A final rank-zero validation extension was followed by rerunning all ten callable frontend/execution checks, including the four build variants. This gives 181 passing test executions across 171 distinct checks. The final collection suites compile 128 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Coverage includes every numeric width, Bool/String/record arrays, mixed arguments, inferred array lengths, fixed, dynamic and rank-zero tensor shapes, local imports, worker-produced collections, repeated awaits and inline map captures.

Raw collection probes execute 2,944 invocations and compare 1,053,184 lanes exactly, including floating-point edge bit patterns, offset/strided borrowed input, immutable snapshots and retained output leases after settlement. The extension also runs 1,664 CLI allocation-failure processes and 100 aggregate-context allocation probes. Checked faults retain their original source locations; already-failed and pre-cancelled contexts do not enter the target. Invalid collection signatures preserve existing executable and IR output artifacts. Existing named-function, frontend, collection, module, task and error regression suites also pass.

The [collection qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-function-collections-2026-10-19.json) records the regression and final callable batches with their corresponding frozen implementation/test hashes. This is focused macOS ARM64 qualification; the remaining callable, closure, transport and platform gaps listed above are unchanged.

### Expected-error signature extension

All 169 selected checks passed on Apple M2 Max, including existing frontend, function-value, collection, map, module, task and expected-error regressions. The new execution suites compile 148 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. They cover all ten numeric scalar widths and numeric tensor signatures, canonical error aliases/unions, independent factory/invocation channels, higher-order calls, local imports, Strings, Bool arrays, records, rank-zero tensors and checked shape refinements.

The suites run 768 CLI allocation-failure processes and 68 private synchronous allocation probes across success, expected-error and checked-fault outcomes. Already-failed and pre-cancelled contexts leave output untouched and perform no allocations. Owned error payload leases survive invocation settlement, and eight task-ownership harness runs check worker pools of one and four threads. Worker-local callables preserve repeated error observations; coordinating functions can keep a callable across an await. Generated synchronous entries are checked for absence of task/group API calls. Invalid signatures preserve existing executable and IR output artifacts.

The [expected-error qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-fallible-functions-2026-10-19.json) records the selected checks, per-variant probes and frozen source hashes. These results qualify the implemented macOS ARM64 subset; they do not establish a public callback ABI, escaping closure support, function transport or performance improvements.

### Managed expression closures

[Managed CPU expression closures](native-closures.md) extend the same callable signatures with anonymous bindings, returned environments and nested immutable captures. The private representation is now a code/environment pair. All 183 selected checks passed after that representation change, including the earlier named, collection and fallible callable suites; see the [closure qualification](native-closures.md#qualification) and its evidence record for the new capture/lifetime coverage.

[Direct anonymous error declarations](anonymous-errors.md) now use these same checked invocation signatures, including inferred success types, canonical error unions and separate factory/returned-function channels.

Current result extension: [proven function-valued CPU worker results](native-worker-function-results.md) now support cached awaits, use after group release and onward submission. Containing values and unproven provenance remain staged.
