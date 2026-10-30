# Native roadmap

Kalandra's current implementation is the native C++23/MLIR/LLVM compiler and
runtime for CPU and Apple Metal GPU execution. Feature coverage is explicit;
shared language syntax does not imply that every CPU feature runs on a GPU.
The old prototype and intermediate implementation diary are not the current
specification. [Design documents](design/) and executable tests define contracts.

## Main milestones

| ID | Area | Current state |
| --- | --- | --- |
| N1 | Native language and values | The qualified subset includes all ten numeric types, immutable collections/tensors, records/enums, declared errors, recursive functions/closures and structured CPU tasks. Preserve documented capture, target, shape and coordinating-call restrictions; this does not claim every accepted language feature is complete. |
| N2 | Apple GPU integration | Complete for the agreed numerical scope: ten numeric types, ranks zero through eight, maps, sums, rank-two products, resident control/dataflow and explicit transfers. Hardware arithmetic is the default where supported; software fallback is explicit. [Completion boundaries](design/native-gpu-completion.md). |
| N3 | Packages | Native project/library manifests, local scopes, explicit Git dependencies, locks, frozen/offline installation and named updates are implemented. Authenticated transport and broader ecosystem work remain separate follow-ups. |
| N4 | Developer tooling | Checker diagnostics, unsaved/incremental analysis, language-server navigation/references/rename/completion/signature help, formatter and VS Code client are implemented for the current subset. [Language server](design/native-language-server.md). |
| N5 | Distribution | Qualified for macOS 26+ ARM64 CPU/Metal, Linux x86-64 and Windows x86-64, including hosted rebuilds, standalone packages and macOS signing/notarization. Each release candidate must rerun qualification. FreeBSD is deferred; Windows TSan is unsupported. |
| N6 | Documentation and release | Native website and 20 Tiramisu documentation pages are implemented; runnable examples are verified. Finish candidate CI/artifact review before a separately authorized release. |

There is no current percentage estimate: the earlier effort estimate predates
completed integration and should not be presented as a release readiness metric.

## Current release candidate

The [release qualification plan](native-ai-performance-plan.md) tracks the active
gates. Optimized full-precision float32 GPU matmul uses a pinned MIT Steel subset
on the qualified M2 Max, with explicit fixed-tree mode. No production MPSGraph or
Accelerate dependency is added. Numerical, ownership, cancellation and package
checks must pass independently of performance.

The [current benchmark index](../benchmarks/README.md) separates equivalent-source
CPU comparisons, isolated kernels and complete models. Large GPU diagnostics are
promising, but identical-build controls failed; exact performance claims and
absence of small-workload regressions are not established. Further optimization
must follow measurement rather than delay an otherwise qualified first release.

## Follow-up work

- Complete remaining accepted-language coverage within the documented subset
  boundaries, without silently relaxing capture, ownership or error contracts.
- Investigate deep tensor-choice compile time and conditional GPU graph bindings.
- Improve parallel-map granularity, closure/environment storage and collection
  sharing where provenance proves safety; preserve ordered results and failures.
- Resolve GPU timing variability before precise performance acceptance, then
  measure command/pass costs and kernel/dataflow fusion opportunities.
- Plan public custom-kernel and external GPU-library interoperation with explicit
  formats, ownership, transfer, numerical and task-completion contracts.

[NVIDIA/CUDA](native-cuda-roadmap.md), [distributed execution](native-distributed-roadmap.md)
and [C/C++ integration](native-interop-roadmap.md) have separate roadmaps. They are
not prerequisites for this CPU/Metal release. Distributed execution still needs
the agreed design discussion before implementation.

Historical reports, rejected experiments and prior milestone estimates are
recoverable through the [research archive](../benchmarks/research-archive.json).
