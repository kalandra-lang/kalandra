# Function values

Current aggregate extension: [proven function-containing CPU worker values](native-worker-callable-aggregates.md) now support records, ordinary enums, function arrays, record arrays and transitive captures. Unproven parameters, expected-error payloads and indirect spawn remain staged. Earlier subset restrictions below describe the preceding increments.

Accepted direction — 2026-10-19. The project owner approved this syntax and staged scope. The [core design](core.md) accepts function types, lexical captures and automatically managed capture lifetimes. [Named CPU function values](native-function-values.md) and [managed expression closures](native-closures.md) are now executable. The examples below describe the complete accepted direction, including those remaining parts. [Function-type parsing](#implementation-status) is also implemented.

## The ordinary case

```kalandra
fn double(value: int) -> int:
  value * 2

fn apply(value: int, transform: fn(int) -> int) -> int:
  transform(value)

fn makeAdder(offset: int) -> fn(int) -> int:
  fn(value: int): value + offset

export fn main() -> int:
  transform = double
  addTen = makeAdder(10)
  apply(transform(3), addTen)
```

The result is `16`. A named function can be bound without calling it. A returned closure keeps the captured `offset` valid after `makeAdder` finishes. Calling either value uses the ordinary call syntax; no wrapper object, capture list or lifetime annotation is required.

## Function types and inference

Use `fn(ParameterType, ...) -> ResultType`. Parameter names belong to declarations and anonymous functions, not their types. Zero parameters use `fn() -> int`. Parameters and captures have no special fixed-count limit; ordinary compiler complexity and runtime allocation limits still apply.

```kalandra
increment: fn(int) -> int = fn(value): value + 1
twice = fn(value: int): value * 2
values = map([1, 2, 3], increment)
```

An expected function type supplies anonymous parameter types. Without that context, annotate the parameters; infer the result from the body. Do not infer a function's parameter types from whichever later call happens to be encountered first. Missing information is a diagnostic, never `Any`.

Function types are structural: equal parameter and result types describe compatible callables even when they capture different data or refer to different declarations. Records and enums inside those signatures retain their existing nominal identities. The initial rule requires exact parameter/result types, including concrete numeric widths and canonical expected-error sets. There is no implicit numeric conversion, parameter/result variance, overload resolution, generic function value or partial application.

Function arrows associate to the right:

```kalandra
fn(int) -> fn(int) -> int
fn(fn(int) -> int, int) -> int
```

The first takes an integer and returns a function. The second takes a function and an integer. Parentheses can group a function type when needed.

## Expected errors remain explicit

`fn(int) -> int ! ValidationError` describes a function whose invocation can return `ValidationError`. An expected error remains a result that the caller must handle or propagate:

```kalandra
fn checkedApply(value: int,
                transform: fn(int) -> int ! ValidationError) -> int ! ValidationError:
  try transform(value)
```

Here `ValidationError` stands for a declared error family. This is a signature/body illustration, not a standalone program.

The error suffix belongs to the immediately preceding result type. Group the returned callable to distinguish a fallible factory from a fallible returned function:

```kalandra
fn make(offset: int) -> (fn(int) -> int) ! BuildError
fn make(offset: int) -> fn(int) -> int ! ValidationError
```

These are interface examples. The first factory can fail while constructing an infallible function. The second factory returns a function whose later invocation can fail. Nested expected-result values are not introduced by this spelling. Error unions and aliases follow the existing [error-channel rules](error-unions.md).

## Captures are immutable managed values

Capture referenced lexical bindings when evaluating the anonymous function, once. Capture nothing else implicitly. Repeated references to the same binding share that capture; shadowing follows ordinary lexical scope. Capturing a value does not consume it or allow mutation through another binding.

Completed CPU scalars, Bool, String, arrays, tensors, records and enums use their existing immutable lifetime rules. Captured function values retain their own environments transitively. Returning a closure, passing it into another function, or keeping it after the creator returns must preserve those values. No stack address may outlive its owner. This does not require an observable deep copy of collection data, nor does it promise a particular allocation strategy.

Do not capture unhandled expected results, child task handles, GPU-resident values, device selectors, or scoped resource capabilities in the first implementation. Handle a fallible result before capturing its completed value. Existing task/resource escape restrictions remain in force; wrapping a handle in a closure must not bypass them.

Closure equality, serialization, a public native callback ABI and a way to inspect the capture environment are outside this proposal.

## Execution boundary for the first implementation

Start with synchronous CPU callables. They can use the supported synchronous CPU operations and return typed expected errors. Both `fn(parameters): expression` and [indented anonymous bodies](native-block-closures.md) are implemented. Indented bodies use immutable local bindings and a final result expression, following the same control-flow rules as named functions.

Named function references must be checked transitively for task/device coordination, including helpers which hide those operations behind error handling. Reject a resumable target with an actionable diagnostic; never treat it as a synchronous native call or block a scheduler worker waiting for its children.

Ordinary calls and `map(values, transform)` must accept the same compatible function value. A function-valued conditional can select between compatible callables with different captures. A pipeline inserts its left-hand value as the first argument under the existing rule. Argument expressions are evaluated once, left to right; creating a closure does not execute its body.

[Function-valued record and enum fields](native-callable-fields.md) now have an implemented same-invocation subset. [Direct arrays of functions](native-function-arrays.md) are also supported. [Capture-free named function arguments](native-worker-functions.md) now cross to statically named CPU workers. [CPU data-capturing closures](native-worker-closures.md) also cross as arguments. [Direct nested function captures](native-worker-nested-closures.md) are also supported. Function-containing aggregate captures and unproven worker function arguments, function-valued captures/results in containing values, indirect `spawn`, and GPU function arguments remain staged implementation gaps, not permanent language limits. Keep statically named worker calls working. A synchronous worker may still construct and invoke a closure internally once that local lowering is qualified. Indirect task invocation must eventually preserve the existing scheduler admission, capture retention, cancellation and settlement contracts. The [local CPU worker transport contract](worker-function-transport.md) now specifies ownership, private metadata and the staged implementation gates; named, CPU data-capturing and direct nested-function argument stages are implemented; broader transport remains staged.

Do not freeze the synchronous-only restriction into an undocumented universal callable ABI. Before supporting resumable function values, extend the compiler's inferred execution information and continuation calling convention. Authors should not need manual effect lists merely to pass a function around. Remote function contracts belong to the separate distributed roadmap and require the agreed design discussion first.

## Implementation and qualification order

1. **Parsing implemented:** recursive function-type syntax and grouped type parsing, with bounded nesting, source spans and clear diagnostics. Distinguish a callable's expected error from a factory's error channel. The syntax and staged scope are accepted.
2. **Named CPU subset implemented:** typed named references, immutable aliases, higher-order parameters/results, compatible branch results and sequential maps preserve exact signatures and nominal identities across local imports. The [current callable subset](native-function-values.md#supported-signatures-and-remaining-gaps) also supports direct CPU collection signatures with checked shapes and ownership. Expected-error callable signatures, canonical error sets, synchronous error handling and fallible factories are implemented.
3. **Expression closures implemented:** returned expression closures use managed environments, nested function captures and anonymous type inference. Keep the representation private. Prefer direct-call specialization where provable, but support runtime selection between compatible functions rather than relying on one statically chosen target.
4. **Managed-lifetime qualification implemented:** the [closure qualification](native-closures.md#qualification) checks ownership and failures before extending transport: returned closures after creator exit, captured arrays/records/Strings, nested closures, repeated invocation, branch-selected targets, typed errors, empty maps, invalid signatures, shadowing, allocation failure and rejected task escapes. Exercise debug, optimized and sanitizer builds. Recheck existing map and local-worker regressions.
5. **Indented bodies implemented:** [anonymous suites](native-block-closures.md) support ordered immutable bindings, nested captures and existing error control. [Direct anonymous error declarations](anonymous-errors.md) are also implemented. [Record and enum fields](native-callable-fields.md) now store managed callables. [Arrays of functions](native-function-arrays.md) now preserve the same signatures and captures. [Direct control-flow map callbacks](native-control-maps.md) share the synchronous checked calling path. [Delimited anonymous suites](native-delimited-closures.md) now support block callbacks directly inside calls and collections. Extend local-worker transport in separate reviewable increments; address resumable calls explicitly. Keep GPU callbacks, distributed contracts and the public C/C++ ABI on their respective roadmaps.

Each implementation increment must state its supported subset. The accepted design is broader than the currently qualified implementation and does not define a public closure representation.

## Implementation status

The native parser accepts recursive `fn(ParameterType, ...) -> ResultType` syntax in parameter, result, binding, field and anonymous-function annotation positions. Arrows associate to the right; parentheses group types. Function-type parameters contain types without parameter names and accept a trailing comma, like ordinary parameter lists. Grouping an ordinary type preserves its semantic identity.

Each callable stores its own error-family spans separately from the enclosing named function's error channel. An ungrouped suffix belongs to the innermost returned callable. To give a factory an error channel, group the returned callable first. Ambiguous consecutive suffixes such as `fn factory() -> fn() -> int ! A ! B` are rejected with `P006`; spell `-> (fn() -> int ! A) ! B`. Error-family unions retain the existing required grouping after `!`.

Recursive syntax owns its nested signatures and preserves source spans, including grouping and error suffixes. Type nesting uses the configured shared syntax-depth bound. A separate type-parsing node budget uses `SyntaxLimits.maxNodes`, including grouped types; the existing expression-node budget remains independent. Exceeding either type bound produces `P036`. There is no special small parameter-count cap.

Parsing alone does not resolve types or make every callable executable. The checker now supports the [named CPU function-value subset](native-function-values.md), including indirect calls, maps and worker-local invocation. Named synchronous expected-error callable signatures and fallible factories are supported. [Function-valued record and enum fields](native-callable-fields.md) are supported within one invocation. Managed returned expression closures are supported. [Indented anonymous bodies](native-block-closures.md) are supported. [Direct anonymous error declarations](anonymous-errors.md) are supported, including inferable success types and independently declared factory/returned-function channels.

The initial parser milestone passed 26 focused checks, including debug and ASan/UBSan frontend tests. Its [historical evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-function-type-syntax-2026-10-19.json) records that syntax-only scope and source hashes. The semantic-boundary tests now reflect the executable named subset while continuing to reject invalid signatures and unsupported storage. See [named function-value qualification](native-function-values.md#qualification) for the subsequent execution milestone. See [managed closure qualification](native-closures.md) for the subsequent returned-closure milestone.

Current result extension: [proven function-valued CPU worker results](native-worker-function-results.md) now support cached awaits, use after group release and onward submission. Containing values and unproven provenance remain staged.

[Inferred execution through function values](native-callable-execution.md) now supplies bounded indirect target sets and transitive continuation/coordination analysis. It preserves unknown sources separately and does not yet enable resumable first-class execution.

[CPU coordinating function values](native-resumable-callables.md) now implement finite proven resumable targets, including captured/returned closures, indirect spawn and bound parallel-map callbacks. This supersedes the initial synchronous-only execution gate described above; recursive coordinating calls now use bounded CPU continuations; unknown sources and GPU callables remain gated.
