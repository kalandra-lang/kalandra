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

### Invocation-local program executor

The private executor now composes the shared forward-only map, balanced-reduction,
matmul, branch/jump, alias and shape instructions on resident typed storage.
All ten numeric types pass independent references in hardware and explicit
software modes on both physical GPUs, with ranks 0/1/3/8, zero geometry and
selected-path faults. Current source artifacts were regenerated before testing.
Typed entry names, physical input projections and exact reflected parameter-word
counts are admitted before execution; malformed schemas are rejected.

One prepared program owns immutable code, while each invocation owns its scalar
captures, value slots, use counts and reduction state. A 257-stage program uses
one TaskCore record and one external capability, checking cancellation between
control steps and completed dispatches. It never blocks a scheduler worker on a
GPU fence. Cancellation/deadline acknowledgement, concurrent invocations, dropped
caller aliases, repeated awaits, arithmetic-before-later-shape fault precedence,
and metadata-budget rejection pass at 1/2/4 workers on NVIDIA and Intel. Scalar
results use the existing ten native numeric task layouts; tensors remain distinct
from rank-zero tensors and scalar values. Capture reservations retire after their
owned metadata and inputs, before external completion acknowledgement.

System Clang and standalone GCC qualification pass. Fully instrumented Intel
TSan passes with the validation layer disabled privately; ordinary tests retain
validation on both GPUs. ASan/UBSan assertions pass without access/UB errors,
while LSan still exits unsuccessfully with the same 2,497-byte/seven-allocation
loader-control finding; this is retained unsuppressed. The existing Metal source
diagnostics, optimized matrix tests and installed-package/relocation test pass
after extracting the shared instruction enum.

This completes the private composed executor slice, not source-language parity.
The source supervisor/CPU descriptor and transfer bridge, transform kernels,
comprehensive conversion/feature/fallback admission, shared registry quotas,
fault injection and distribution remain required before exposing a CLI target.
No cancellation contract or supported production target changed in this slice.

### Complete-source supervisor and explicit transfers

The common host continuation emitter is now shared by Metal and a private Vulkan
source compiler. Metal's command/build driver remains separate; its placed
supervisor and shader control remain byte-identical. Vulkan placements always
use the typed numerical program path, producing SPIR-V and native CPU exports
instead of Metal artifacts. This is a private complete-source qualification path,
not a released CLI target or completed distribution milestone.

Explicit uploads and downloads run through managed external completion. CPU
imports validate the invocation-owned descriptor, retain immutable backing,
respect offsets and charge the full allocation rather than just the visible
subview. CPU exports copy into the receiving invocation and reject device or
foreign-registry backing. Owned host snapshots and device allocations share one
registry-wide byte quota across physical contexts. Prepared definitions and
invocation captures share an argument quota; pending admission is likewise
shared across device completion services. Reservations retire after their
backing. No transfer or numerical fence blocks a scheduler worker.

The transfer harness passes all ten types, every byte pattern, ranks 0/1/3/8,
empty domains, invalid descriptors, producer teardown, aggregate quotas and
rollback, and held upload/download cancellation/deadlines at 1/2/4 workers on
NVIDIA and Intel. The composed-program harness also checks a CPU descriptor →
upload → 257-stage resident program → download → independently owned CPU result.
Source-adapter tests cover each selected device, dropped prepared-plan/kernel
aliases, a registry-wide single-operation limit, and foreign-registry rejection.
Clang and GCC checks pass. Fully instrumented Intel TSan adapter, transfer and
program checks pass with the separately diagnosed validation layer disabled.

The same 67 source-program cases used by Metal pass through Vulkan at 1/2/4
workers on both physical GPUs in both hardware and explicit software arithmetic. Independent
expected records cover all ten basic type cases and expanded int8/float32/
float64 reductions, feedback, branches, local shape obligations, conversions and
fault precedence. The native policy includes actual hardware float64 on both
devices. These finite source references do not replace the still-pending
comprehensive floating-point special-value/conversion admission below. No performance acceptance is inferred from these
correctness runs.

Instrumented registry assertions pass without access/UB reports, while LSan
still exits unsuccessfully: 5,090 bytes / 14 allocations in the latest multi-
registry test, with DBus and unloaded-module stacks. Its DBus retained byte
count differs from the earlier two-initialization loader control; matching
allocation classes/counts do not prove complete attribution. This finding is
retained without suppressions, alongside the earlier transfer/program findings.
Transforms, automatic fallback selection, broader numerical/capability and
failure-injection coverage, and relocatable CLI packages remain unfinished.

The mixed CPU/GPU source fixture also passes both arithmetic modes on both GPUs
at 1/2/4 workers: native CPU matmul/reduction tasks, captured tensor/scalar CPU
exports, repeated uploads/downloads, GPU scaling, then a CPU tensor result.
Its exact result matches the existing Metal fixture oracle, `[52,54,64,66]`.
Metal's optimized source-program suite and installed-package/relocation check
pass after sharing the supervisor; normal and instrumented frontend diagnostics
also pass. These checks retain Metal behavior while Vulkan integration proceeds.

### Hosted source artifacts and stable device selection

Hosted Vulkan qualification now builds the shared supervisor and runtime with the
unchanged production-pinned LLVM/MLIR 23.1.3 toolchain, compiles complete placed
and mixed CPU/GPU source programs in both policies, and validates all ten types'
map/reduction/matmul SPIR-V. That source job and the standalone GCC Debug/Release
jobs pass. Hosted checks compile artifacts; physical execution remains the
separate NVIDIA/Intel evidence above. PR updates trigger one Vulkan run rather
than duplicate push/PR runs for the same branch.

The source device registry retains Vulkan device UUIDs when opening contexts,
so a changed enumeration index cannot silently select another GPU. Explicit
identity selection and missing-identity rejection pass on both physical devices;
Clang/GCC runtime, numerical and source-adapter regressions pass. Device UUIDs
are not printed in qualification output.

### Predefined transforms and native precision

The next slice integrates all ten bitwise rank-2 transpose types and native
float32 softmaxRows/rmsNormRows with the private complete-source task pipeline.
Transpose adapts the pinned original MIT ggml tile, with preserved 64-bit payloads,
rounded tails, a two-dimensional group grid and empty metadata. Row operations
reuse the owned Metal adjacent-pair algorithm; unchanged upstream ggml row
kernels were rejected because their reduction order and +infinity behavior differ.
Host/device epsilon and invalid-epsilon-before-empty fault order are preserved.
The row artifacts require Vulkan 1.2 and advertised float32 NaN/infinity/signed-
zero preservation; unsupported capability returns 106 before pipeline creation.

Independent qualification passes 190 cases / 37,323 cells per physical GPU;
all ten transpose types are bit-exact. Twenty-four complete source programs pass
on NVIDIA and Intel at workers 1/2/4 (144 executions): hardware/software
transpose, native row operations, repeated awaits, device-computed epsilon and
arithmetic failure. Managed transform checks pass scratch quota rollback, held
cancellation/deadlines, cancellation over injected completion failure, and exact
retirement of private backing at those worker counts. No scheduling or
cancellation contract changes. The unchanged CPU transform oracle passes
156,006 checks; the physical Metal transform regression passes (130.10 seconds).

The initial exact CPU/Metal RMS oracle failed NVIDIA's `[-3]`, epsilon 1e-5 case:
expected `bf7ffff7`, observed `bf7ffff8`. Intermediate readback found a one-ULP
native sqrt difference. Precise local storage did not change it; an experimental
integer midpoint correction exposed approximate native division and was rejected.
The accepted hardware profile instead propagates Khronos SPIR-V bounds through
the actual adjacent-pair tree: correctly rounded operations with implementation-
defined rounding, division 2.5 ULP in its guaranteed range, exp
`(3+2*abs(x))` ULP, and sqrt inherited from reciprocal of a 2-ULP inverse sqrt.
The profile is explicit and separate from the unchanged CPU/Metal oracle;
transpose payloads, NaN behavior and invalid-epsilon faults remain strong checks.
Eight independent oracle tests include out-of-bound corruption rejection.

Requesting explicit RTE additionally caused a reproducible NVIDIA 595.112.07
SIGILL during pipeline compilation. The debugger identifies libnvidia-gpucomp;
Intel accepts that artifact. The preservation-only shader passes both GPUs.
Explicit RTE and the integer sqrt correction are not shipped. Original failed
artifacts and debugger/control logs are retained in the isolated qualification
workspace. No exact-bit or speed claims are inferred from the native profile.

GCC 16.2.1 standalone Release passes all nine registered checks, including the
new ABI/provenance/precision audit and managed transforms. Fully instrumented
Intel ThreadSanitizer transform tests pass with the independently diagnosed
validation-layer issue excluded by the existing private no-validation switch;
normal NVIDIA/Intel tests retain validation. ASan/UBSan assertions pass, but
unsuppressed LeakSanitizer still fails at exit (2,497 bytes / 7 allocations,
including DBus/unloaded-module stacks), matching the earlier single-loader
control's counts, not proving every stack is attributed. No suppression added.

### On-demand additional devices

The source registry now initializes its default service for host imports and
structured rejection, then opens other physical contexts only when selected.
Enumeration retains all device identities without requiring unrelated contexts
to initialize successfully. A mutex publishes a complete driver/completion-service
pair once per index; contexts never move or get replaced after publication.
Concurrent downloads and conformance hold controls synchronize with creation,
and a newly opened service inherits an existing hold. All contexts continue to
share byte, argument and pending-operation ledgers. Default-device failure still
fails registry creation; this does not yet decouple host admission from that
service.

Eight concurrent selectors publish one additional context, invalid selection
creates none, and the complete registry tests pass Clang/GCC and the isolated
Intel TSan profile. Eight complete-source variants explicitly select each
physical GPU, covering placed/mixed CPU-GPU work in hardware/software modes at
workers 1/2/4 (24 executions). ASan/UBSan assertions pass; exit-time LSan remains
unsuppressed (5,066 bytes / 14 allocations), with external-loader attribution
limits unchanged. The preceding transform commit also passes all three hosted
Vulkan jobs, including production-pinned compiler/artifact qualification.


### Native-first explicit arithmetic fallback

Source bundles now carry a native module and an ABI-compatible GPU integer
float64 alternative when `hardware-with-software-fallback` is requested.
Native loads first; only Unsupported capability can select the alternative.
Missing/corrupt primary artifacts and device failures never trigger fallback;
a missing backup retains Unsupported and a corrupt backup remains Invalid.
Native float32 instructions/comparisons are retained in the fallback variant.
The initial all-width lowering failed at a float32 comparison and was rejected;
a mixed-width pass regression also verifies the existing software behavior and
fail-closed unsupported widths. Default CPU/Metal lowering remains unchanged.

A private test ceiling omits float64 from VkDeviceCreateInfo, ensuring tests run
the software GPU implementation rather than accidentally using available native
float64. Hardware-only correctly rejects under that ceiling. Native-enabled
controls prefer hardware; explicit fallback passes 6,032 independent IEEE
arithmetic checks per GPU across add/subtract/multiply/divide (including
canonical arithmetic NaNs). The full 67-case graph suite passes both GPUs at workers 1/2/4
with float64 disabled (402 executions), covering selected shapes/branches,
reductions, feedback, conversions and fault precedence. Twelve full-source
policy/operation variants pass 72 source executions, including 24 expected
hardware-only capability rejections. Repeated awaits and CPU transfers remain
explicit. GCC and Intel's separately documented TSan configuration pass;
CPU/Metal arithmetic-policy and frontend regressions pass (55.73 seconds).

ASan/UBSan assertions pass, but LSan still fails unsuppressed at exit with
28,563 bytes / 77 allocations in this repeated-registry fixture. An independent
control with eleven direct Vulkan-loader initializations and no Kalandra code
reproduces exactly that total. Allocation-class/mapping evidence is retained;
matching totals strengthen attribution but do not constitute a leak-clean run.
The prior lazy-context commit also passes all three hosted Vulkan jobs.

Automatic fallback is now implemented for unsupported float64 arithmetic;
integer capability ceilings, native row-math limitations, remaining numerical
edge/conversion coverage, fault injection and released CLI/distribution work
remain open. Earlier entries describing fallback as unimplemented record the
state at those earlier checkpoints, not the current implementation.

Ten-type native/fallback map/reduction/matrix artifact bundles validate. The
existing independent reduction/matrix/overflow/empty/tail primitive suite passes
on both GPUs with native float64 disabled.

### Native float64 casts and complete conversion/ReLU qualification

The cast audit found a shared Metal restriction that forced float64 conversions
through integer code even on capable Vulkan devices. A backend capability option
now enables native double casts for Vulkan, retaining checked bounds and the
unchanged Metal/default behavior. Checked GPU branches carry integer bits;
the Vulkan fallback pass removes double cast opcodes before strict arithmetic
lowering, keeping native float32 operations/comparisons intact. Narrow integer
native cast carriers are explicitly canonicalized. No runtime/task ABI change.

Independent APFloat/APInt tests cover all 100 pairs with exhaustive narrow
integers, floating exponent fields/boundaries and deterministic random data.
Native/software/fallback on each physical GPU passes 2,008,620 cells (12,051,720
total) with input immutability and inactive guards. Native profiles each admit
136 documented device NaN/FTZ results; software/fallback profiles admit zero.
Actual SPIR-V audits confirm native double casts and double-free fallback while
retaining native binary32 casts. These checks establish emitted driver-native
operations, not unmeasured silicon performance.

All 100 pairs also pass complete rank-3 source tasks under native and fallback
policies. Checked overflow, repeated awaits, downloads and an invalid uniform
cast before empty output add coverage: 212 programs / 1,272 executions across
both GPUs at workers 1/2/4. Thirty full-source ReLU policy/type programs pass
another 180 executions with native float64 disabled. Independent bitwise ReLU
corpora pass 202,792 cells per policy/device (1,216,752 total), including NaN
payloads, signed zeros, signed minima, unsigned identity, empty domains and tails.

The default Metal proof generator's float32 AND float64 CPU/GPU artifacts remain
byte-identical. Existing Metal frontend, full numerical-program and arithmetic
policy checks pass (128.13 seconds). ASan/UBSan assertions pass for conversion and
ReLU; isolated Intel TSan checks pass. Unsuppressed LSan exit findings remain:
2,497 bytes/7 allocations for conversions and 2,509 bytes/7 for ReLU. The 12-byte
ReLU difference is retained without claiming an exact loader-control match.
Hosted qualification now compiles/audits all conversion policies and representative
complete-source casts/ReLU without a software GPU substitute.
