# CPU conditionals and short-circuit Boolean logic

The native CPU language supports value-producing `if`/`else` expressions and the Boolean operators `and`, `or` and `not`. Conditions and logical operands must be completed CPU `Bool` values. Numbers, task handles, unobserved expected-error results and tensors do not acquire implicit truthiness.

```kalandra
fn choose(value: int64, limit: int64) -> int64:
  if value > limit:
    value
  else:
    limit
```

An `if` evaluates its condition once, then exactly one branch. Each branch ends in a result expression; `else` is required because the expression must produce a value on every path. Each branch has a lexical scope. Its bindings do not escape or change outer bindings. Nested branches may shadow outer names; an initializer still sees the outer binding before introducing its own name.

Inline branch results are also supported:

```kalandra
fn choose(flag: Bool, left: int32, right: int32) -> int32:
  if flag: left
  else: right
```

Use another `if` inside `else` for additional cases. There is no separate `elif`, optional/Unit result or implicit no-else statement in this subset.

## Types and shapes

Both branches need a common success type: the same builtin/numeric identity, scalar-versus-tensor category, tensor rank and residency. There is no branch-driven numeric promotion. An explicit result context may type numeric literals directly, following the existing literal rules. Already typed values keep their concrete types.

When matching tensor ranks/types have different known extents, the result retains common extents and uses dynamic extents where they differ. An explicit static result/annotation still checks each branch against its requirements. Dynamic shape checks and allocations run only on the selected path. Branch inputs remain immutable; returned tensors retain valid storage after branch locals and workers finish.

Both branches are checked even when a condition is a Boolean literal. Unknown names, invalid types, known arithmetic failures and unobserved tasks are not accepted merely because their branch appears unreachable. This milestone does not add a general frontend constant evaluator for conditional results.

## Boolean operators

- `left and right` evaluates left first. If it is false, return false without evaluating right. Otherwise evaluate right and return its Bool value.
- `left or right` evaluates left first. If it is true, return true without evaluating right. Otherwise evaluate right and return its Bool value.
- `not value` evaluates value once and returns the opposite Bool.

Precedence, from tighter to looser, is calls/arithmetic unary signs, multiplicative arithmetic, additive arithmetic, pipelines, comparisons, `not`, `and`, `or`, then catch. Thus `not a == b` means `not (a == b)`, and `a or b and c` means `a or (b and c)`. Parentheses can always make grouping explicit. Comparison chaining remains rejected.

The words `if`, `else`, `and`, `or` and `not` are reserved in binding/function/parameter/handler/pattern positions. Use `not`, not `!`, for Boolean negation; `! TaskError` retains its separate return-channel spelling.

A skipped operand/branch performs no calls, allocation, task submission or expected-error propagation. An executed operand retains its normal checked faults, source locations and cancellation behavior. Static validation still checks skipped source code.

## Tasks and expected errors

Task operations may appear inside a branch. A selected branch can spawn work, suspend at await and resume using the existing bounded scheduler. Task handles cannot escape as conditional results. A task created within a branch must be observed there. A task created before a conditional must be observed on every path, or observed unconditionally outside the conditional. The initial checker applies this rule even when the condition is a literal.

Short-circuiting is also conditional execution. An await in only the right operand of `and`/`or` does not discharge a task created before that expression: the right operand may be skipped. Await it first, or observe it on both paths of an explicit conditional.

Branches can return compatible success values with an expected TaskError channel. Their joined result keeps that channel; use `try` or `catch` explicitly. `fail error` also supports a concrete Bool success context, including inside a conditional error handler. Logical operands themselves must be completed Bools; an explicit `try` in a potentially executed operand propagates errors only when that operand runs. Errors and owned data use the existing propagation/recovery and lifetime contracts.

## Native lowering and limits

Ordinary synchronous conditionals lower to native MLIR `scf.if` control flow without introducing scheduler entries. Their branch frames are separate, so branch-local values cannot be reused across non-dominating paths. Tensor bufferization and checked invocation ownership remain shared.

Task-bearing programs use a Boolean branch instruction in the common continuation planner, followed by the existing value-copy/join paths. Only the selected instruction stream executes. GPU/mixed-target Boolean control flow remains gated by the existing Bool capability restriction. Bool tensors, general pattern matching, broader values/modules and public native library packaging remain separate work. Native distributed execution still requires the agreed design discussion.

## Qualification

The [frontend checks](../../native/tests/ControlFlowTests.cpp) cover result types, lexical scopes, precedence, shape joins, malformed/omitted branches, reserved words, task obligations, forged condition IR and CPU/GPU capability boundaries. The [compiled driver](../../native/cmake/VerifyControlFlow.cmake) qualifies debug, optimized, ASan/UBSan and TSan builds.

The [scalar harness](../../native/tests/ControlFlowRuntimeTests.cpp) verifies exact selected bits across all six numeric types under eight ARM floating environments. The [logical](../../native/tests/ControlLogicRuntimeTests.cpp) and [skipped-call](../../native/tests/ControlSkipRuntimeTests.cpp) harnesses use dynamic inputs to check truth tables, precedence and executed-versus-skipped faults. [Allocation checks](../../native/tests/ControlAllocationRuntimeTests.cpp) ensure skipped tensor work performs no allocation, and [tensor result checks](../../native/tests/ControlTensorRuntimeTests.cpp) cover differing extents, exact retained values, allocation failures and settled owners.

Worker probes cover both branch paths, nested branch-local groups and cached awaits, skipped task submission, Boolean error recovery/rethrow, allocation injection, cancellation/deadlines/admission, and retained matrix results for all six numeric types with one, two and four workers. Existing numeric, Boolean, frontend, task and Metal regressions are qualified alongside the new cases.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-control-flow-2026-10-17.json) records 85 focused checks passing across debug, optimized, ASan/UBSan and TSan builds: 3,145,728 exact scalar selections, 936 worker harness runs, 192 tensor allocation/lifetime invocations and 640 exact retained tensor lane checks. All 640 tracked float32 numerical artifacts remain unchanged, along with the arithmetic/conversion engines, checked invocation ABI, scheduler and storage runtime. This is focused qualification rather than the entire 349-test suite.
