# Vulkan backend implementation

Tracking: #263. Vulkan work starts from develop, independently of the temporary billion-loop evidence branch.

## Delivery sequence

1. Opt-in runtime foundation: explicit physical-device selection, capability reporting, owned buffers, memory visibility, descriptor/pipeline management, bounded submission and fence-based resource settlement. Run validation-layer tests on real Linux NVIDIA hardware. Standalone runtime builds do not require installing compiler dependencies on the test machine.
2. Audit and pin permissively licensed upstream numerical shaders. Port numerical implementations rather than recreate optimized kernels. Preserve per-file provenance/notices and test actual compiled SPIR-V. Start with float32; do not imply support for unqualified types or reduced-precision operations.
3. Compiler-generated Vulkan artifacts, source placement/transfer/task integration, explicit unsupported-capability diagnostics, checked arithmetic and floating-point policy coverage. No silent CPU fallback.
4. Structured cancellation/deadlines, fault precedence, concurrent invocations, bounded resources and device-loss/failure tests. Submitted resources remain owned through actual device completion; cancellation does not terminate a submitted kernel.
5. Distribution/CI integration, runnable user docs and complete application qualification. Audit shapes/tails, empty inputs, numerical references and cleanup; performance acceptance uses uncontended hardware and equivalent work.

## Qualification environment

Linux x86-64, openSUSE Tumbleweed (20261028), RTX 5090 with NVIDIA 595.112.07, Vulkan device API 1.4.329. Intel integrated graphics is also available as a second hardware path. Host tools report Clang 23.1.4, CMake 4.4.4 and Vulkan headers 1.4.363. This standalone runtime qualification does not change the compiler's pinned LLVM requirement.

The user owns system software installation: do not install packages, drivers, SDKs or other tools on the Linux host without asking. Use an isolated source/build directory and do not modify the other agent's game checkout or processes. Sample GPU load before tests; performance claims require a separately coordinated idle window. Mac MoltenVK results, if collected, are portability evidence rather than native NVIDIA qualification.

## Current status

Draft implementation. No released Vulkan backend or CLI support is claimed. Keep the PR draft until the complete source-to-device path and the acceptance criteria in #263 are qualified.
