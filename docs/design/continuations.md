# Common CPU/GPU continuation planning

CPU task lowering and the mixed Apple GPU supervisor now use one planner over checked typed IR. The planner decides value slots, lexical groups, branch edges, recovery exits and suspension points. The LLVM CPU emitter and C++ Metal supervisor emitter implement those instructions through their respective value layouts and runtime interfaces.

## Representation and policies

[Continuation.h](../../native/include/kalandra/Continuation.h) defines a module plan containing typed frame slots, parameters, group indices and instruction lists. Calls and operations retain checked function/value identities rather than source names. The input `TypedModule` must remain alive and unchanged through planning and emission; the plan keeps a reference to it for numerical operation metadata and diagnostics. Planning does not query hardware or execute source code, and failure returns no partial plan.

The shared instruction set covers literals and arithmetic, numerical kernels, synchronous calls, selectors, transfers, owned child admissions, await, group open/join, copy, try/fail, catch, cases, branches, field reads and return. Tensor slots retain rank, extents and residency; task and expected-error status remain separate type facts.

Two call policies preserve the current backend interfaces:

- Ordinary CPU task compilation imports eligible non-suspending helper call graphs into native kernels, including recursive helpers and function values. A named synchronous child retains its outer task plan and transport, while its source calls use the native checked ABI. Expected-error/control-flow callables and coordinating helpers retain checked continuation storage. If native physical-frame admission fails, compilation retries the previous continuation representation. See [native task subgraphs and ownership](checked-call-optimization.md#native-subgraphs-inside-cpu-tasks) for the optimization boundary.
- Mixed-program compilation preserves ordinary numeric calls as checked target dispatches, expands resumable helpers into the caller, and plans its selected `main` plus reachable resumable child entries. CPU calls use the existing checked export ABI; GPU calls and transfers use the Metal task runtime. A placed spawn contains evaluated argument slots plus a checked selector slot. Constant tensor data stays embedded instead of consuming one frame slot per literal element; nonconstant operands retain their evaluation order. Logical function IDs map explicitly to CPU export IDs. Other function IDs retain their positions rather than being renumbered around the selected entry.

These are call-lowering policies within the same control-flow planner. They retain the existing value types, scheduler and GPU capabilities.

Native numeric work runs synchronously on its worker until return, fault or
cancellation. It does not yield to sibling continuations at every source call;
await/join and coordinating continuations remain scheduler suspension points.
The bounded worker scheduler is not a preemptive executor for pure CPU work.

## Settlement and control flow

A parallel operation opens a lexical child group. Its join instruction closes the group, suspends until all children and external completions settle, then publishes its value or error. Every `try` or `fail` edge targeting that boundary goes through the same join. A function or handler boundary instead stores the expected error and branches to its common exit.

Catch copies a successful value directly to the continuation after the handler, or binds the failure and enters the handler. Match arms receive independent value/symbol environments; their results join in one output slot. Pattern fields consume the already evaluated subject. Shared graph values are reused only where their defining environment is available. Exhaustive matches still fault on an invalid internal tag.

Arguments are planned before an explicit task selector and admission. Reading an existing immutable selector requires no new runtime work. Transfers retain their owning group, checked target and tensor shape. The runtime still performs device availability, capability, budget and binding checks.

The Metal emitter maps logical plan positions to target program counters. A group join uses a close step followed by a resumable wait step; resuming the wait does not close the group again. If admission observes an already failed containing group, it branches to that group's join and preserves the original failure. It does not convert the failed admission handle into a different fault.

CPU and Metal value storage remain backend-specific. CPU expected results use the existing checked layout; the Metal frame uses scalar/Buffer/Failure fields and immutable backing leases. The target emitter can retain shape facts proven by its kernels even when the public result type is dynamic. Straight-line writes preserve these facts, while branch writes keep an extent only when every successful payload path agrees; explicit checked extents remain constraints. Scalar float32 negation in the supervisor flips the sign bit directly. No floating-point reassociation, automatic tensor transfer or mutable payload sharing is added.

## Inspection and qualification

```sh
# CPU task plans, including helper expansion and synthetic builtin entries.
build/native/native/kalandra-check --dump-plan native/tests/fixtures/task-recovery.k

# Placement-aware entry plan; no GPU is needed to inspect it.
build/native/native/kalandra-check --placement --dump-plan native/metal-source/recovery.k
```

`--dump-plan` and `--dump-ir` are mutually exclusive. Placement plan inspection selects `main`. The Metal compiler writes the exact same `continuation.plan` alongside its typed IR, generated supervisor and kernel artifacts. It is an inspection format, not a stable serialized program or supported compiler input. Symlink and source/output collision preflight cover this artifact too.

[Planner tests](../../native/tests/ContinuationTests.cpp) check 22 mixed and two CPU fixtures normally and under ASan/UBSan. They verify group/failure/branch edges, typed targets and transfers, argument order for fresh selectors, deterministic output, CPU placement rejection, entry validation, recursion limits, synthetic builtin child plans and a 21,000-element embedded literal that must not exhaust continuation slots. [Artifact checks](../../native/metal-source/VerifyPlacement.cmake) compare 70 target plans with independent standalone planning. Existing compiled CPU/Metal tests exercise the common plan with worker variation, shape faults, recovery, cancellation/deadlines, admission races, allocation/completion failures and cleanup. Debug, optimized, ASan/UBSan and separate TSan variants remain required.

The private generated GPU-await/group observation points let the lifecycle harness cancel a containing scope at a GPU wait without depending on a frame-field number or source program counter. They are not a public runtime ABI. [Evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/common-continuations-2026-10-16.json) records the regression and artifact comparisons.

## Resumable mixed helper bodies

A directly called helper can now open parallel scopes, submit CPU/GPU work, wait for transfers and tasks, catch settled failures, and return a completed host scalar or tensor. Helpers retain explicit float32 or rank-one/rank-two float32 tensor interfaces. A declared `! TaskError` channel is required when an expected error escapes; a helper that fully handles its errors can have an ordinary result signature.

```kalandra
fn scale(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  values * factor

fn transform(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input = try await transfer(values, gpu)
    result = try await spawn scale(input, factor) on gpu
    try await transfer(result, devices.cpu())

fn main() -> Tensor[float32, ?] ! TaskError:
  try transform(tensor([1, 2, 3]), 2)
```

The planner classifies direct control/task effects and propagates that classification through the resolved call graph. This includes nonfallible wrappers around helpers that catch their errors. The Metal compiler excludes those functions from the synchronous CPU export module. Pure numeric callees remain CPU exports or eligible GPU kernels.

Each expanded invocation gets separate value slots, symbols, groups and a function error boundary. `try` exits that function boundary with an expected error; it does not skip settlement of any owning parallel scope. The caller can propagate or catch the result. A helper's own scope nests under the caller's active group when one exists. Main can also directly call a helper which owns its work without opening an outer scope itself. Repeated calls get distinct frame storage. Existing recursion/depth and frame-size limits bound expansion.

Task handles and resident GPU tensors still cannot escape as function results. Selectors are not public parameter/result types. Fixed/dynamic shape checks and explicit transfers still apply. A source helper is not automatically a shader: placed functions still must satisfy the numeric GPU capability checks.

Direct calls suspend their containing invocation; independently scheduled calls now use the child entries described below. Broader language values, element types and devices remain separate work. Native distributed execution still requires its agreed design review.

The [helper fixture](../../native/metal-source/resumable-helpers.k) exercises transitive helper calls, CPU tasks, GPU work and returned host tensors. It reuses the full recovery lifecycle harness in debug, optimized, ASan/UBSan and TSan builds: allocation-failure sweeps, held completions, scope/root cancellation and deadlines, failed completion recovery, bounded admission and budget rejection, with zero retained resources after cleanup. [CLI tests](../../native/metal-source/VerifyResumableHelpers.cmake) cover successful and failed helper paths, repeated calls, helper-owned scopes, deferred errors, identical standalone plans and normal/sanitized compiler emission, and transactional recursion/escape diagnostics and a spawned helper acceptance case. [Qualification evidence](../../native/metal-proof/evidence/resumable-helpers-2026-10-16.json) records the complete regression run.

## Independently scheduled mixed helpers

A coordinating helper can now be spawned as its own task, including from another coordinating helper:

```kalandra
fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    work = spawn transform(tensor([1, 2, 3]), 2)
    try await work
```

Here `transform` is the helper above. Its task runs on the CPU and submits the explicitly placed GPU kernel. Unplaced spawn and `on devices.cpu()` have the same coordinator semantics. Placing a coordinating helper itself on a GPU is rejected: a GPU entry must be an eligible numeric kernel. No implicit transfers or CPU fallback are added.

The shared planner emits a separate plan for every reachable resumable child, preserving original function IDs. Each entry is emitted once even when several call sites use it. Synchronous numeric calls retain their existing ABI; direct coordinating calls retain expansion into the caller. Recursive child-entry cycles and excessive plan expansion remain diagnostic.

Each generated child entry has its own parameter packet, frame, program counter, function error boundary and lexical groups. Construction copies scalar arguments, retains immutable host tensor leases, and owns its shader-directory string before the submitting packet disappears. Aliased tensor arguments retain the same immutable backing without copying the tensor data. The existing bounded scheduler owns admission, suspension, result publication and frame destruction. A helper can await CPU children, GPU kernels, transfers and further coordinating children without blocking a worker on those waits, including with a one-worker pool.

Declared fixed parameter dimensions are checked in the child before its body executes. Dynamic operation guards remain at their source operations, including guards inside protected scopes: moving those guards to entry could bypass a helper's own error handler. Completed results use the existing scalar/Buffer layouts. Repeated observations retain the same immutable result backing, which can outlive child-frame retirement. A failure is exposed only after owned scopes and external completions settle. Catching a failed helper uses the same enclosing parallel boundary as other child failures.

Parent cancellation reaches suspended helper descendants and their GPU completions. Independent invocations remain isolated. The scheduler retains its existing deadline behavior: the cancelled group records `DeadlineExceeded`, while descendants report cancellation. Depth, record and retained-frame budgets apply across the whole task tree. No new scheduler, transport or public runtime ABI is introduced.

[The spawned-helper fixture](../../native/metal-source/spawned-helpers.k) runs sibling coordinators, nested coordinating children, ordinary CPU children, explicit GPU kernels and host tensor results. [Lifecycle tests](../../native/metal-source/SpawnedHelperTests.cpp) run in debug, optimized, ASan/UBSan and TSan builds with 1/2/4 workers. They check capture and result lifetimes, aliased arguments, fixed-shape faults, task-core/child-invocation/Metal allocation failures, cancellation and deadlines during held GPU completions, failed completions, invocation isolation and admission limits, with zero retained resources after retirement.

[CLI acceptance](../../native/metal-source/VerifySpawnedHelpers.cmake) additionally covers settled parent recovery, a nonfallible recovering child, scalar and matrix results/captures, empty tensors, dynamic shape recovery inside the child, matching standalone plans, normal/instrumented compiler equivalence and transactional capability, recursion and escape diagnostics. [Qualification evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/spawned-helpers-2026-10-16.json) records the regression and unchanged numerical artifacts.

The implemented interfaces still use float32 scalars and rank-one/rank-two float32 host tensors. General values, modules and additional numeric families remain ahead. Shared element-type descriptions are the prerequisite for extending the numerical subset without duplicating scheduler and placement machinery. Distributed execution remains behind its separate design review.
