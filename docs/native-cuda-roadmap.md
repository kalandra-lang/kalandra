# Native NVIDIA/CUDA roadmap

NVIDIA/CUDA support is a separate roadmap from the main native language release. It remains a project goal, with the NVIDIA RTX 5090 on Windows as the first hardware qualification target. Apple Metal GPU execution stays in the main roadmap. CUDA implementation, platform integration and hardware qualification are excluded from the main completion estimate and release gates.

## Scope

Run Kalandra GPU work on NVIDIA devices while preserving the accepted immutable values, explicit device selection/transfers, structured tasks, checked failures and numerical contracts. Reuse the typed IR, execution planning and ownership machinery where their contracts apply. Metal support is a foundation, not evidence of CUDA compatibility.

Windows CPU compiler/runtime support is distinct from CUDA support. Linux CUDA qualification follows the initial Windows hardware target. Neither GPU target blocks the main native roadmap.

## Milestones

1. **Backend and capability contract.** Define supported element types, operations, ranks and device limits, and how unsupported programs are diagnosed. Keep device-specific launch and binding details behind the existing source model.
2. **Compiler lowering.** Implement and verify the MLIR/LLVM NVIDIA lowering path, kernel generation, launch argument layouts and device artifact loading. Preserve checked arithmetic and floating-point semantics; qualify hardware instructions or software lowering against the language contract.
3. **Device runtime and storage.** Implement device discovery/selection, managed buffers, explicit uploads/downloads, bounded command submission and completion tracking. Define allocation failures, device loss and storage ownership through completion.
4. **Structured task integration.** Connect CUDA completion to the shared scheduler, including nested CPU/GPU work, repeated awaits, cancellation, deadlines, admission limits and cleanup. Cancellation must not free storage still used by a submitted GPU command.
5. **Windows toolchain and hardware qualification.** Establish reproducible compiler/runtime builds and dependencies. Run differential numerical, shape, ownership, failure and concurrency checks on the RTX 5090. Measure performance separately from correctness.
6. **Linux qualification and delivery.** Verify the Linux toolchain/runtime, supported driver/device combinations, packaging, diagnostics and complete application examples. Document the qualified capability matrix and remaining limits.

Status: planned; no CUDA implementation or supported device/platform combination is claimed here. Track progress independently from the [main native roadmap](roadmap.md#native-implementation-status). Distributed execution and public C/C++ integration keep their own separate roadmaps.
