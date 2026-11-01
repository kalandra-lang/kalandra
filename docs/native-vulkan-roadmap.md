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

## 2026-11-01: first runtime slice

- Added an opt-in standalone C++23 Vulkan runtime; default compiler builds and supported CLI targets are unchanged.
- Imported the original MIT ggml addition/tiled matmul shaders and their 11-file textual dependency set, pinned at `a91eab67d8389d979d29fdf6633b2edd0718eb4b`. Full license and per-file SHA-256 hashes are retained. No Steel/CuTe dependencies were imported. Both built SPIR-V modules validate for Vulkan 1.1 and declare only Shader capability / float32 arithmetic.
- Debug/Clang 23.1.4 and Release/GCC 16.2.1 hardware correctness/validation passed on NVIDIA RTX 5090 and Intel Graphics (ARL). Cases cover exact addition, resident chaining, non-tile-aligned matmul against independent double references, a full-precision operand probe, zero shapes/contraction, immutable copied inputs, foreign-context rejection, allocation-budget rollback, concurrent callers and resource retention after the runtime facade dies. This is runtime bring-up evidence, not source-language qualification or benchmark performance.
- Initial tests rejected two host ABI mistakes (addition push constants are 116 bytes, and the matmul split width must be the contraction extent). Both were fixed; upstream shaders remain unmodified.
- Sanitized correctness executes successfully but LeakSanitizer fails at process exit (initially 4,850 bytes / 14 allocations, with DBus and unloaded-module stacks). An independent loader-only control with no Kalandra runtime also fails (2,485 bytes / 7 allocations). Selecting NVIDIA alone and disabling implicit desktop layers process-locally did not eliminate the reports (one run 5,042 bytes / 14 allocations). No broad leak suppressions or sanitizer disabling were added. Leak-clean sanitizer qualification remains unresolved; the control is retained to investigate external initialization/teardown separately.
- No packages/drivers/SDKs were installed by the agent on the Linux machine. No game processes were modified. Source/build files live in an isolated directory. No performance acceptance timings were collected.

Next: finish teardown/sanitizer attribution, then introduce compiler-generated Vulkan artifacts and asynchronous task integration before expanding numerical/type coverage. This synchronous private bring-up has no cancellation contract and must not be exposed as the completed language backend. The existing source-language cancellation/fault rules remain unchanged.
