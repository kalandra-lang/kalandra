# Inferred execution through function values

Kalandra's callable type describes its parameters, completed result and expected-error channel. It does not require an author-written effect list. Before enabling function values whose bodies coordinate tasks, the compiler must discover which functions an indirect call can execute and propagate their execution requirements to callers.

This milestone introduced the analysis foundation. [CPU resumable callable dispatch](native-resumable-callables.md) now uses that information to execute finite proven coordinating targets. Recursive coordinating graphs now use bounded CPU continuations. Unknown target sets and GPU function values remain gated.

## May-call graph

The compiler reuses the bounded whole-module value-flow graph used for worker provenance. Each indirect invocation has a sorted set of compatible possible function targets and a separate unknown-source flag. Known targets and unknown sources can coexist; a partial target set is not a proof that no other target exists.

The graph follows immutable bindings, parameters, direct and indirect factory results, captured callables, conditionals, record/enum fields, function arrays, collection projections and supported expected-error outcomes. Aggregate projections conservatively join possible fields/elements. Callable edges require matching signatures, including numeric types, nominal record identities, collection contracts and expected-error channels. An unknown callee considers compatible referenced targets and propagates uncertainty to their parameters.

Task/device coordination analysis consumes this graph; continuation analysis also inherits those coordination requirements. Synchronous checked callable entries encapsulate their own expected-error control flow, which does not make their indirect callers suspend. Calling an indirect target adds execution edges from its possible targets to the invoking function. These requirements propagate through direct helpers, maps and further indirect calls to a fixed point, including recursive higher-order graphs.

Constructing, capturing or returning a function does not execute its body. Coordination analysis therefore does not add an execution edge merely because a function reference exists. Continuation representation analysis may still require a checked callable entry to construct a control-flow function value; that is a separate implementation requirement.

## Unknown sources and analysis limits

Unknown is distinct from known task/device coordination. For example, a private generated map adapter receives a synchronous callback through a checked calling boundary; its isolated typed module need not contain the original caller. Classifying every unknown source as a coordinating target would incorrectly reject that adapter.

The coordination bit alone is not a general proof that an arbitrary function value can run synchronously. The current source capability gate requires complete proven target sets for coordinating indirect calls. Resumable dispatch must consume the unknown-source information as well as the known target set, and validate the selected entry at the execution boundary.

The analysis has a configurable work budget, with the existing default of 300,000 graph steps. Exhaustion returns no flow proof rather than exposing partially computed targets. Continuation/effect classification treats affected indirect callers conservatively when analysis fails. This does not enable an unchecked fallback entry or silently choose one candidate.

## Dispatch and remaining work

[CPU callable dispatch](native-resumable-callables.md) now distinguishes synchronous and coordinating entries, preserves selected managed environments and suspends/resumes finite proven calls without blocking workers. Unknown targets and malformed entries retain checked failure paths. Recursive coordinating graphs and placement-aware/GPU callables remain gated; distributed contracts and the public C/C++ callback ABI have separate roadmaps.

## Qualification

Focused tests cover aliases, higher-order parameters, returned functions, conditional selection, record fields, indexed and sliced function arrays, returned function arrays, captured callbacks and recursive forwarding. They substitute a same-signature coordinating target into otherwise checked IR to verify transitive effects and verify that proven recursive coordinating graphs now plan through bounded child entries. Tests also check signature filtering, deterministic target sets, function creation versus execution, unknown parameters, budget exhaustion and sequential maps whose callbacks handle expected errors. Debug and ASan/UBSan variants exercise the analysis; existing execution and worker suites qualify supported behavior separately.

**75/75 selected checks passed**, including the new debug/ASan/UBSan analysis checks and existing debug, optimized, ASan/UBSan and TSan execution regressions. The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-callable-execution-analysis-2026-10-20.json) lists the exact selection and source hashes. This is focused qualification, not the full project suite.
