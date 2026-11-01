# Native NVIDIA/CUDA roadmap

NVIDIA/CUDA support is a separate roadmap from the main native language release. It remains a project goal, with Linux x86-64 / NVIDIA RTX 5090 as the first hardware qualification target, as requested for this implementation. Apple Metal GPU execution stays in the main roadmap. CUDA progress is tracked separately from the main completion estimate. Future releases that include a CUDA archive require exact-tag package provenance, reproducible composition and relocated qualification.

## Scope

Run Kalandra GPU work on NVIDIA devices while preserving the accepted immutable values, explicit device selection/transfers, structured tasks, checked failures and numerical contracts. Reuse the typed IR, execution planning and ownership machinery where their contracts apply. Metal support is a foundation, not evidence of CUDA compatibility.

Windows CPU compiler/runtime support is distinct from CUDA support. Windows CUDA remains a subsequent platform qualification; macOS stays Metal-only. Windows CUDA is outside this Linux milestone.

## Milestones

1. **Backend and capability contract.** Define supported element types, operations, ranks and device limits, and how unsupported programs are diagnosed. Keep device-specific launch and binding details behind the existing source model.
2. **Compiler lowering.** Implement and verify the MLIR/LLVM NVIDIA lowering path, kernel generation, launch argument layouts and device artifact loading. Preserve checked arithmetic and floating-point semantics; qualify hardware instructions or software lowering against the language contract.
3. **Device runtime and storage.** Implement device discovery/selection, managed buffers, explicit uploads/downloads, bounded command submission and completion tracking. Define allocation failures, device loss and storage ownership through completion.
4. **Structured task integration.** Connect CUDA completion to the shared scheduler, including nested CPU/GPU work, repeated awaits, cancellation, deadlines, admission limits and cleanup. Cancellation must not free storage still used by a submitted GPU command.
5. **Linux toolchain and hardware qualification.** Establish reproducible compiler/runtime builds and dependencies. Run differential numerical, shape, ownership, failure and concurrency checks on the RTX 5090. Measure performance separately from correctness.
6. **Linux delivery and subsequent platform qualification.** Verify the Linux toolchain/runtime, supported driver/device combinations, packaging, diagnostics and complete application examples. Document the qualified capability matrix and remaining limits.

Status: experimental implementation on `codex/cuda-linux`; the opt-in Linux compiler accepts `--target cuda`. Direct NVVM/PTX lowering and the Driver API service run all ten numeric types, ranks 0–8, structured graphs, transfers, reductions, matrix products and tensor transforms on the RTX 5090. Private and public-command suites cover numerical conversions, explicit software companions, cancellation/deadline acknowledgement, checked faults and ownership. The MIT warp-tiled float32 route is selected only by explicit optimized matmul policy; shape/capability companions retain native fixed-tree GPU arithmetic. CUDA's row-major matrix-layout trait does not change the source API or cancellation contract. The downloaded production-pinned LLVM 23.1.3 archive has passed physical application, relocation, missing-resource and independent numerical qualification. Two controlled resident kernel series support the tile64 choice; cuBLAS remains faster. Whole-process measurements failed their stability controls and support no end-to-end speedup claim. Broader GPUs, older drivers and Windows CUDA remain unqualified. CUDA is not included in v0.99.1. See `native/cuda/README.md` for reproducible contributor commands.

## Linux implementation sequence

- Confirm the owner-installed CUDA toolkit, driver, headers, sanitizer tools and host compiler. System installation remains owner-managed; keep test sources/builds isolated from the other agent's game.
- Lower shared typed GPU IR directly through NVVM to PTX with pinned LLVM 23.1.3; the existing Linux development profile may use matching private LLVM/MLIR 23.1.4. Preserve separate float multiply/add by default. Do not implicitly link vendor numerical libraries or select TF32/half inputs.
- Version and reflect the launch ABI. Initial artifact tools retain complete one-dimensional memref descriptors, including pointer, offset, size and stride. Kernel launches must verify parameter offsets/sizes and resource bounds; flattened storage does not remove source tensor ranks.
- Add a lazily resolved CUDA Driver API runtime with private contexts, streams/events, immutable device storage and actual completion ownership. No context reset, driver changes or interference with other GPU users.
- Integrate the shared source supervisor, all ten numeric types/ranks 0–8, graphs/transforms, explicit transfers and TaskCore cancellation/fault precedence. Native arithmetic stays native where qualified; software lowering is explicit.
- Audit/pin permissive upstream numerical kernels before import. Separate native fixed-tree arithmetic from explicitly optimized full-float32 products. Keep original licenses/hashes and record any rejected precision shortcuts.
- Qualify independent numerical oracles, software/native policies, shape/tail/empty cases, memory/resource faults and concurrent cancellation. Retain submitted resources until real completion. Audit the signed/unsigned count-to-index boundary and large-domain admission when converting 32-bit GPU counts to 64-bit NVVM indices.
- Package a relocatable Linux compiler with owned PTX lowering resources and system-driver-only execution; then run complete downloaded-package applications and honest resident/transfer-inclusive benchmarks. Reuse CPU target runtime data without claiming GPU cross-compilation or Windows CUDA qualification. Track progress independently from the [main native roadmap](roadmap.md#native-implementation-status). Distributed execution and public C/C++ integration keep their own separate roadmaps.
