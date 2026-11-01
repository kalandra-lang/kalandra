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

## Metal-parity completion boundary

The user requested complete feature parity before this work is considered done.
The draft status is not lifted by a successful primitive or kernel-only probe.

- [x] Initial pinned MIT ggml float32 primitive import and physical NVIDIA/Intel validation.
- [x] Shared typed GPU map/program IR emitter linked by Metal and Vulkan artifact tools; original Metal placed supervisor/kernel outputs remain byte-identical and diagnostics pass.
- [x] Actual source-derived pointwise kernels for all ten numeric types: normal exact arithmetic, integer overflow flags, packed tails/empty domains on both physical GPUs.
- [x] First managed generic dispatch adapter using existing TaskCore completion capabilities: worker counts 1/2/4, held acknowledgement cancellation/deadlines, device-failure classification/precedence, bounded captures, cached results, facade/core teardown, and escaped result ownership. This does not yet constitute a full source task backend.
- [ ] Full source compiler/supervisor and explicit CPU/GPU transfer/CPU descriptor bridge.
- [ ] Complete shapes/ranks 0..8, maps/scalar lifting, all conversions, reductions, matmul policies, branch/shape/error control and resident graph composition.
- [ ] Transpose, ReLU, softmaxRows and rmsNormRows with existing numerical/fault contracts.
- [ ] Real hardware feature admission and explicit arithmetic fallback, comprehensive independent numerical oracles and same-source Metal parity fixtures.
- [ ] Allocation/submission/completion fault injection, concurrent ownership/resource bounds and ASan/UBSan/TSan qualification with external loader findings resolved or precisely attributed.
- [ ] Relocatable CLI/distributions, editor/docs/install updates, reproducible hosted checks and complete user applications.

### Linux development compiler

The owner requested retaining system Clang 23.1.4. The official matching
LLVM/MLIR 23.1.4 archive was downloaded, SHA-256 verified and extracted only in
the dedicated project test directory. The owner installed all system packages:
LLVM/toml++ development files, then zlib/zstd/libxml2 development files. The agent
installed no system package, driver, SDK or compiler. The full compiler and Vulkan
artifact tools built with `/usr/bin/clang(++)` 23.1.4 and matching private MLIR;
the normal production 23.1.3 lock is unchanged. This opt-in development profile
explicitly disables installation rules and uses installed shared Zstd to adapt
the official archive's static target reference. It is not release qualification.

### Arithmetic oracle correction retained

The initial source-generated hardware float32 test compared against a gradual-
underflow CPU oracle and failed at `0x00000001 * 0.5 + 0x00000001`: NVIDIA returned
zero while the unrestricted IEEE oracle expected `0x00000001`. Existing Metal
hardware qualification explicitly applies FTZ at arithmetic boundaries. The
hardware test now uses that same documented policy (including signed-zero and
NaN handling); explicit software arithmetic retains the independent gradual-
underflow oracle. Both modes pass the edge/empty/tail cases on NVIDIA and Intel.
This changes no existing arithmetic policy and is not a claim that native
hardware mode preserves gradual underflow.

### Compiler kernels and task completion qualification

The private Vulkan artifact tools now emit, lower and validate actual source
map programs through the shared GPU IR. Both native hardware and explicit
software arithmetic pass all ten type chains on RTX 5090 and Intel ARL. Checks
include exact normal arithmetic, first-lane integer overflow, empty domains,
packed narrow tails and immutable input bytes. Hardware float64 instructions
execute on both devices; comprehensive special-value/conversion qualification
and automatic fallback artifact selection remain pending.

The versioned flattened-grid ABI also passes a 4,194,257-element float32 map
chain on both GPUs in both modes. Every output is checked, including the
partially occupied final group and extra physical groups. Overflow of padded
uint32 lane indices is rejected before allocating or submitting. This removes
the portable 65,535 X-group ceiling without changing Metal-generated kernels.

Managed dispatch qualification passes at 1/2/4 scheduler workers on both GPUs:
owned copied captures survive caller alias teardown, cached outputs outlive the
producing facade/core, cancellation/deadlines retain access until acknowledgement,
and cancellation wins an injected device-completion fault. Bounded admission
produces a structured Budget fault while the scheduler stays live. Concurrent
immutable readback uses per-allocation host synchronization rather than holding
the queue's GPU-wait mutex. Fresh uploads and pipeline creation likewise avoid
unrelated fence waits. Vulkan validation reports zero errors.

Both system Clang 23.1.4/full compiler and standalone GCC 16.2.1 builds pass the
runtime/interface/provenance/development-profile/async checks. The original
Metal placed host and kernel artifacts remain byte-identical after factoring
the shared emitter. These are qualified foundation changes, not completion of
the unchecked source-supervisor and distribution milestones above. The existing
loader-only LeakSanitizer finding remains unresolved and is not suppressed.

### Typed primitives and NVIDIA compiler failure

Actual source-generated balanced reductions and row-major fixed-tree matrix
kernels now pass all ten types on both physical GPUs in hardware and explicit
software modes. Independent exact small-integer references cover odd contraction
sizes, non-tile-aligned outputs, empty geometry/contraction, input immutability
and checked overflow. This is kernel conformance; selected-path source program
composition, shape admission and cancellation checkpoints still need integration.

The first matrix harness used M/N/K carrier order rather than the shared shader's
M/K/N ABI; its independent reference rejected the outputs, and the harness was
corrected. The next run exposed a real NVIDIA 595.112.07 shader-compiler SIGILL
while creating the uint32 matrix pipeline. Intel accepted the same validated
artifact. SSA rewriting, standard SPIR-V optimization, disabling pipeline
optimization, removing redundant high partial products, and narrowing private
stack storage did not resolve the trap; those experiments are not retained in
production code. Native uint32 low/high multiplication does resolve it, while
preserving the low result word and checking the high word for overflow. The
shared emitter now uses that instruction for uint32 multiplication. Both GPUs
also pass 81 boundary-pair and 128 deterministic random-product cases against a
uint64 CPU oracle. Metal's optimized CLI numerical-matrix regression passes
(98 source programs, each executed at worker counts 1/2/4), including typed
overflow, resident sums, zero geometry and balanced floating-point products.

Automatic software-fallback bundling remains unimplemented, so private Vulkan
tools explicitly reject that policy instead of silently emitting a native-only
artifact labelled as fallback-enabled.

### Unsuppressed sanitizer investigation

The new ASan/UBSan runtime and managed-task checks complete their correctness,
cancellation and ownership assertions without memory-access/UB reports. Exit-time
LeakSanitizer still returns failure. Capturing module mappings before unload
identifies loader-control allocation sites in DBus (`+0x2bd17`, `+0x3b79c`),
Mesa EGL (`+0x252fd`) and libdrm (`+0x84ed`). One loader-only initialization and
the managed-task test each report 2,497 bytes / seven allocations; two independent
loader-only initializations and the runtime test each report 5,066 bytes / 14
allocations. A late snapshot can contain remapped addresses, so matching counts
are supporting evidence rather than proof for every historical stack frame.
No suppressions, sanitizer disabling or artificial library retention were added.
The diagnostic `--maps` options and loader-only `--maps-twice` reproduce this
investigation without linking the Kalandra runtime into the control.

An initial separately instrumented ThreadSanitizer managed-task run crashes
inside the sanitizer with a nested SEGV; the loader-only TSan control passes.
The debugger locates this in TSan allocation interception on an NVIDIA EGL
driver-created thread, before a Kalandra task worker appears in the stack.
Intel-only TSan completes but reports an internal validation-layer deletion /
rwlock-unlock race at `vkWaitForFences`. Separate fully instrumented Intel runtime
and managed-task runs pass with no TSan reports when that layer is explicitly
disabled. Their commands select the installed physical Intel driver, retain all
Kalandra instrumentation and use the private test-only `--no-validation` flag;
Vulkan validation stays enabled in the ordinary hardware qualification above.
No suppressions or production/default build changes were introduced. NVIDIA
TSan compatibility and validation-layer concurrency attribution remain open.
