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

Draft implementation. The opt-in development CLI is available; installed release packages remain unfinished. Keep the PR draft until the complete source-to-device path and the acceptance criteria in #263 are qualified.

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
- [x] Full development source compiler/supervisor and explicit CPU/GPU transfer/CPU descriptor bridge.
- [x] Complete shapes/ranks 0..8, maps/scalar lifting, all conversions, reductions, matmul policies, branch/shape/error control and resident graph composition.
- [x] Transpose, ReLU, softmaxRows and rmsNormRows with existing numerical/fault contracts.
- [x] Real hardware feature admission and explicit arithmetic fallback, comprehensive independent numerical oracles and same-source Metal parity fixtures.
- [x] Allocation/submission/completion fault injection, concurrent ownership/resource bounds and ASan/UBSan/TSan qualification with external loader findings resolved or precisely attributed.
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

### Embedded resources and lazy primitive pipelines

The raw runtime archive now contains all 14 validated built-in SPIR-V modules.
The generator accepts only the closed built-in set and emits deterministic words,
without absolute paths or timestamps. A data-only object is independently usable
by compiler tools without linking the Vulkan loader. Round-trip tests extract
every module byte-for-byte, check order/path independence, and reject missing,
repeated or malformed resource inputs. No generated shader arrays enter Git.

Addition/matrix pipelines initialize transactionally on first nonempty use under
the existing queue mutex. Partial Vulkan objects are destroyed on failure before
publication; later calls can retry. Their own limits are checked there: addition
uses 256 threads/116 push bytes; the selected matrix tile uses 128 threads/64 push
bytes and 17,408 shared bytes. Unrelated kernels no longer inherit the former
global 256-thread/20-KiB admission gate. Empty and zero-contraction results retain
their previous behavior. No numerical, cancellation or task-ABI contract changes.

Standalone GCC's ten tests pass. With all 14 original built-in files temporarily
absent, a copied executable passes exact additions and independent matrix
references on RTX 5090 and Intel ARL, including concurrent cold pipeline creation,
tails, empty domains and ownership cleanup, with zero validation errors. This
proves raw runtime resource relocation; it does not qualify installed compiler
packages or eliminate the per-application generated numerical artifacts.

Isolated Intel TSan raw runtime checks pass. Both physical GPUs pass ASan/UBSan
correctness assertions without access/UB reports; unsuppressed exit-time LSan
still fails (5,078 bytes/14 allocations on NVIDIA, 5,066 bytes/14 on Intel). These
totals are retained, not treated as leak-clean or an exact prior-control match.

Hosted source qualification caught an indentation error in the committed
conversion fixture generator after the earlier local source run. The fixture
was corrected explicitly; current-head hosted and physical source runs are
required before counting this slice as fully qualified. The rejected CI run
41274313326 remains evidence; its source build did not pass.

The corrected generator subsequently passed all 242 programs / 1,452 physical
executions, including conversions, invalid uniform/empty-domain cases and ReLU,
on both GPUs at workers 1/2/4. These artifacts used the previously qualified
compiler/runtime build; later runtime changes have their own checks below.

### Safe allocation/submission/completion failure qualification

One-shot private injection points cover buffer creation, memory allocation/bind/
map, pipeline layout/creation, descriptor/command allocation, command begin/end,
fence creation, queue submission and fence wait. Injection follows the existing
error paths with partially constructed resources retained for ordinary cleanup.
Submission is rejected before calling the driver; a synthetic failed wait is
reported only after a real fence has completed. No device hang/reset is induced.
Queue submission/wait failures retain the existing poisoned-context behavior;
recoverable allocation/preparation failures retry and return exact results.

All 13 raw points plus generic compiler-kernel pipeline rollback pass on both
physical GPUs. Four failure classes × workers 1/2/4 × ordinary/cancel/deadline
give 36 managed cases per device. Held acknowledgements retain input captures;
cancellation wins pending faults, group deadlines remain distinct from cancelled
child tasks, and task/adapter/byte ledgers retire completely. No task contract or
fault-precedence change was needed. Eleven registered GCC tests pass.

The initial repeated-context test exposed NVIDIA ICD unload/reload static-TLS
exhaustion. Its validation failure is retained; the selected test keeps a separate
empty context alive across independent logical contexts, as the language device
registry already does. Validation messages are not filtered. Repeated complete
loader teardown remains an external compatibility limitation, not a passed case.

ASan/UBSan raw/managed assertions pass on both devices without access/UB reports;
unsuppressed LSan still fails at 2,509 bytes/7 allocations on each device. Isolated
Intel TSan passes the same suite with the already documented validation-layer
isolation. These are not leak-clean or NVIDIA TSan claims.

### Development CLI and executable-relative artifacts

The actual opt-in `kalandra --target vulkan` command now compiles source or project
manifests without the Python qualification adapter. It uses configured native
host C++ tools, shared supervisor/CPU exports, the existing lower/extract stages,
explicit native/software/fallback modules and embedded transform resources.
Compiler startup has no Vulkan-loader dependency; ordinary CPU compilation and
execution still work with an unavailable ICD. This is development integration,
not installed one-download distribution or cross-target GPU compilation.

Public `-O0`/`-O2`, all arithmetic policies, CPU/GPU mixing and project builds are
checked. Eleven real-command builds pass 66 executions across both GPUs/workers
1/2/4, including transpose, softmax and checked overflow. Six negative cases
exercise unsupported sanitizer/optimized-matmul/CPU-cross flags and symlink/hard
link source-output protection. The initial collision fixture used `.cpp` as the
Kalandra source and correctly failed the extension gate; the corrected fixture
uses a valid `.k` source hard-linked to the generated `host.cpp` output.

The generated Vulkan host resolves its executable location via platform APIs,
rather than treating argv[0] as a path. The language loader consumes UTF-8 paths
and reports conversion errors; Metal output remains byte-identical. Linux and
macOS helper tests cover Unicode/spaces, relative, PATH and symlink launches.
Three complete Vulkan programs (resident arithmetic, mixed CPU tasks and a
feature-disabled float64 fallback cast) pass another 90 physical executions with
their original artifact directories absent, across five launch modes on both
GPUs at workers 1/2/4. Twelve registered standalone GCC tests pass. Windows helper
code is implemented but not yet qualified on Windows.

The common compiler builds on macOS, Metal frontend diagnostics pass, and the
unchanged Metal placed supervisor is byte-identical. CPU smoke programs produce
42 on macOS/Linux. The production-pinned hosted workflow now builds the actual
compiler and checks all public command artifacts/output protections without
substituting a software GPU. Current-head results must still finish before
claiming that hosted check passed.

Remaining: installed/runtime variant packages, agreed platform/driver matrix,
optimized matmul routing and broader native floating arithmetic qualification,
end-to-end performance evidence and release-facing user documentation. Explicit
unsupported-mode errors are retained until these paths actually exist.

### Public Linux host ASan/UBSan variant

Normal Linux root builds now provide separate instrumented raw, program, async
and language archives with the matching sanitized CPU core. Public `--sanitize`
selects those archives, instruments generated host C++, and marks CPU exports
with LLVM SanitizeAddress attributes before compilation. The normal compiler
itself remains uninstrumented and keeps its loader-independent startup. GPU
instructions are not ASan-instrumented. Windows sanitizer variants and public
TSan remain explicitly unsupported; private instrumented-root proof profiles
retain their existing role rather than linking mismatched runtime variants.

Two actual public source builds pass twelve numerical/ownership executions
across both devices at workers 1/2/4. Binary symbols and CPU export attributes
confirm instrumentation selection. Intel exits cleanly in all six cases; NVIDIA
has unsuppressed exit-time LSan failures at 5,082 bytes/14 allocations in all six.
There are no access/UB reports. Those exit failures remain open, and the checked-in
driver records their full output rather than suppressing them.
Two independent Vulkan-loader initializations with no Kalandra code reproduce
the exact NVIDIA total and Intel clean exit; this strengthens external attribution
without turning the NVIDIA runs into clean sanitizer exits. Normal mode is
rechecked separately: eleven builds/66 executions and six negative cases pass.
Hosted qualification compiles instrumented applications and checks symbols/IR;
it does not silently execute a substitute GPU.

### Explicit source tiled matrix policy and primary ABI validation

The development command now accepts explicit optimized matmul. Float32 stages
carry a closed compiler descriptor; the builder retains the ordinary fixed-tree
SPIR-V companion and selects the exact embedded, provenance-audited MIT ggml
module. Default remains fixed-tree. Other types and software arithmetic retain
their previous algorithms. No Steel/CuTe or reduced-precision variants are used.
The primitive is unchanged; source execution adds a GPU right-operand transpose,
then multiplication inside the same managed task. This adds a dispatch and a
full right-operand allocation within the existing quota. It may exhaust the
quota sooner than fixed-tree; no claim of a speedup or default policy change is
made without end-to-end measurements. Unsupported tile limits select the native
fixed-tree companion, independently of numerical software-fallback permission.

Logical entry names, input counts and parameter words are now checked before
feature admission. A valid but wrong logical primary can no longer report an
unsupported feature and be hidden by a valid numerical alternative. A mutated
entry-point control proves the difference, including physically disabled FP64.
The shared contract helper is used by both loading and program validation.

The physical matrix corpus passes 32 geometry/algorithm combinations, ten
admission cases and twelve managed cases per device on NVIDIA RTX 5090 and Intel
ARL. It covers tile tails, K=4097, empty/singleton shapes, independent double
references, a full-float32 operand probe, unchanged inputs, concurrent built-in
publication and 200 aliases under the 64-kernel quota. Managed cases exercise
workers 1/2/4, cancellation/deadline after layout completion and lazy multiplication
pipeline failure, with no leaked task capabilities/buffer ledger entries or
validation errors. Sixty actual ten-type source matrix programs pass at workers
1/2/4 on both GPUs (360 executions). The ABI regression also passes 98 source
cast/ReLU cases on both GPUs (588 executions), plus all four 1,508-case IEEE FP64
software corpora per device, retaining their independent oracles.

An isolated Intel TSan matrix run passes. ASan/UBSan runs execute the matrix corpus
successfully on both enumerated devices with no access/UB findings, but each
exits unsuccessfully with unsuppressed LSan reports of 2,509 bytes/7 allocations
in DBus/unloaded-module stacks. Unlike the prior isolated-ICD public source runs,
these tests enumerate both drivers; this does not establish leak-clean Intel
qualification. Full findings are retained, pending isolated-loader controls.

Follow-up isolated-ICD controls resolve the matrix attribution more precisely:
Intel's complete matrix corpus and independent loader control both exit cleanly.
NVIDIA's matrix corpus and one-initialization loader-only control both exit with
exactly 2,509 bytes/7 allocations. All 67 shared source graph cases pass again on
both GPUs at workers 1/2/4 (402 executions). The public command checks 15 builds,
90 physical executions and six output/error protections, including optimized
hardware/native-first products and software fixed-tree retention. Three public
ASan/UBSan applications (resident arithmetic, mixed CPU tasks and optimized
matmul) produce exact results in 18 executions: nine Intel clean exits and nine
NVIDIA failures at 5,082 bytes/14 allocations, matching the retained two-loader
control. No invalid access/UB report or leak suppression is involved.

The broad development build also exposed an unrelated stale private test forward
declaration from cross-compilation: RecursiveSourceTests omitted the CpuTarget
argument of lowerTaskNative. The declaration now matches production. This fixes
a test link failure without changing production lowering; the rejected broad
build log remains retained. macOS frontend and executable recursive-source
regressions pass with the corrected declaration.
The repaired complete Linux development build finishes, and Linux frontend/
executable recursive-source regressions both pass. The latest isolated Intel
TSan run includes the ten algorithm-admission cases and remains clean. All
twelve registered standalone GCC tests pass. macOS shared compiler, Metal
frontend diagnostics and artifact regressions pass. These targeted checks do
not assert that the entire CPU test suite was executed.

### System-loader boundary for GPU application distribution

Generated Vulkan applications and the raw runtime now resolve the OS Vulkan
loader dynamically instead of linking its SDK/import library. Every used core
entry is resolved into an immutable table before any Vulkan entry is invoked.
Concurrent first initialization uses C++ static publication. Missing or incomplete
loaders return Unavailable (104); malformed loader admission cannot reach a
partial table. Linux uses libvulkan.so.1 through normal OS search. The Windows
implementation restricts vulkan-1.dll loading to System32; Windows qualification
is still pending. This does not supply a driver, SDK or GPU compiler package.

One successfully opened OS-loader handle is process lifetime, equivalent to the
previous ordinary loader import. This explicitly preserves dispatch pointers for
captures owned by unrelated static TaskCore objects at shutdown. It does not pin
individual ICDs, devices, instances or buffers. Failed table publication closes
the handle. No external leak suppression or artificial ICD retention is added.

Thirteen standalone tests pass, including eight concurrent callers repeatedly
attempting enumeration/context creation against missing and incomplete loaders.
An incomplete fixture aborts if vkCreateInstance is entered; admission rejects it
without entry. ELF audits confirm no Vulkan NEEDED import in either the probe
or complete generated application. Both application failure modes produce exactly
`Vulkan unavailable: 104`, before touching a GPU. Hosted checks perform these
negative tests without claiming substitute-device execution.

Both GPUs still pass the matrix/admission/managed corpus. Actual CLI applications
pass 15 builds/90 physical executions and six negative/output protections after
the loader change. Public ASan/UBSan again checks three applications/18 executions:
Intel clean, NVIDIA unchanged 5,082 bytes/14 exit-time allocations. The isolated
matrix sanitizer run is Intel clean and NVIDIA unchanged 2,509 bytes/7 allocations;
private Intel TSan also passes. The loader change does not erase vendor findings.

Hosted run 41281315132 rejected the preceding matrix commit under GCC 13 Release
with a maybe-uninitialized diagnostic in move-assignment of optional/vector
schema metadata; GCC 16/Clang had passed. Loaded kernels now construct their
schema directly. Warnings remain errors; the rejected log is retained. Both
physical matrix/admission corpora and the Clang compiler build pass this change;
exact new-head hosted qualification remains required.

### Installed Vulkan package candidate (qualification pending)

A separately compiled `kalandra-vulkan-installed` driver resolves its adjacent
private tools, Clang resource directory, host headers and runtime archives. There
is no installed fallback to configured build-tree resources or ambient PATH.
The optional NativeVulkanCompiler component retains CPU compilation, adds owned
lower/extract/mlir-opt/mlir-translate/spirv-val tools and preserves ggml/validator
notices. The ELF bundler audits the additional private tools, retains the original
validator digest and records its final digest after RPATH relocation. No Vulkan
loader/driver/system SDK library is redistributed. System C++ development
headers/runtime remain required, as a platform SDK is required for Metal.

The development-profile driver compiles with system Clang 23.1.4; its installation
guard remains unchanged and no local installation/staging was performed. The
refactored development path passes another 15 builds/90 physical executions and
six negative/output protections. The unchanged CPU archive's macOS install/
relocation regression passes after the optional packager extension.

Production-pinned CI will build and stage the candidate, audit its ELF closure,
compile after Unicode/space relocation, run a native CPU program and missing-
loader application checks, remove seven owned resources individually to prove
failure without build-tree fallback, and compare two archive digests from the
same frozen build. This is an implementation awaiting those gates, not qualified
installed GPU distribution. The resulting draft artifact must then be downloaded
into the isolated Linux project for physical GPU recompilation/execution.
Windows/ARM platform packages and integration with the five CPU target-runtime
packs remain separate qualification work; no readiness/release claim is made.

The shared Metal/Vulkan compiler explicitly restricts GPU row math to native
float32. Float64/software softmax/RMS are rejected by the shared source contract
on both backends; adding them is not needed to match current Metal support.
Current committed loader/matrix head 6fe71632 passes all three hosted Vulkan jobs,
including production-pinned source compilation. Broader CPU/platform workflows
are still queued/running and must be audited at the eventual current head.

Post-product source control and higher-rank acceptance also pass: twenty matrix
control programs across all ten types, plus 54 rank-3..8 programs, both physical
GPUs at workers 1/2/4 (444 executions). These retain the shared Metal source
fixtures and exact selected-output/shape-fault oracles; Metal defaults are unchanged.

The first production package run (41282620845) compiled/staged the installed
component, then failed because the ELF bundler assumed libexec/kalandra did not
already exist. The GPU component legitimately creates that directory for its
owned tools. The bundler now reuses it; the rejected log is retained. No archive
or installed-distribution qualification is claimed from that failed run. The
install-relative driver additionally respects explicit GNUInstallDirs tool paths.
The next run (41283224494) built and audited the archive, then correctly rejected
it at the first installed GPU compilation: CMake had called the CPU-only driver
"up-to-date" when the GPU component tried to replace the same staged filename.
Stripping the CPU destination changes timestamps; timestamps cannot distinguish
the two source binaries. GPU package staging now explicitly removes that one
owned staged driver after installing CPU resources, before publishing the GPU
variant. This also applies to the existing Metal package composition. The failed
archive is not qualified; installed GPU compilation remains a mandatory gate.
The existing installed Metal package regression passes after the staged-driver
replacement fix. The next Vulkan run (41284357547) now passes relocated CPU
execution and all fifteen installed GPU program builds/output protections. It
then catches a qualification-helper mistake: the loader fixture invoked bundled
Clang directly without its relocated resource directory. The compiler itself
already supplies that directory correctly. The helper now accepts the explicit
owned resource directory as well. The failed helper run is retained; archive
readiness still waits for all missing-resource and reproducibility checks.


### Native float64 preservation and division admission

The original native corpus exposed genuine device differences. RTX 5090 passes
all 6,032 native arithmetic cases. Intel ARL initially flushes subnormal add/sub
inputs/results, loses signed zero on division and miscomputes tiny divisors.
Requesting advertised SignedZeroInfNanPreserve64 and DenormPreserve64 makes
Intel add/sub/mul pass the same exact CPU oracle (4,524 cases). [Vulkan's denormal preservation guarantee](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)
explicitly excludes OpFDiv; feature presence does not
prove correctly rounded division. No RTE modes are used.

A rejected prototype scaled native division operands with powers of two. It
fixed some tiny-divisor classes but still returned 0x0010000000000000 instead of
0x0010000000000001 for min-normal / (1 - 2^-53). Its source and failed output
remain in the private qualification evidence; it is not production lowering.
The independent oracle has not been relaxed to accept this one-ULP error.

The runtime conservatively rejects native scalar float64 division on Intel
before pipeline creation, after validating the logical artifact ABI. Hardware
mode returns Unsupported; explicit hardware-with-software-fallback can select
the existing checked integer GPU companion. This restriction applies to Intel
vendors conservatively; the tested failing device is ARL, not every Intel GPU.
It does not affect native float64 add/sub/mul, native NVIDIA division, float32,
or CPU arithmetic. Numerical fallback is explicit and remains on the GPU.
SPIR-V reflection identifies scalar division by its result type, with malformed
instructions rejected. Native float64 controls are deterministic, bounded and
idempotent; real SPIR-V normalization passes spirv-val without changing the ABI.

Physical per-operation qualification passes 6,032 native + 6,032 software cases
on NVIDIA; Intel passes 4,524 native and 6,032 software cases, plus explicit
hardware-only division rejection and explicit fallback selection. Each operation
also retains disabled-device-feature and artifact-error controls. All fourteen
standalone CTests pass. All 100 conversion pairs pass again: 12,051,720 APFloat/APInt checks across
both devices and three policies, with the same 136 explicitly allowed native
NaN/float32-FTZ results per device and none for software/fallback. The 67 shared
source graph cases pass under explicit native-first fallback on both GPUs at
workers 1/2/4 (402 executions). Root compiler build and recursive CPU source
regressions also pass. Current-head hosted qualification remains pending.

The existing private ASan/UBSan configuration also passes both normalization/
reflection tests and all four physical Intel native/fallback policy probes,
with clean exits and no suppressions. NVIDIA sanitizer findings remain the
separately attributed loader findings above; they are not relabelled clean.


### Installed Linux archive and CPU runtime-data composition

Production-pinned run 41285165375 (a797cdf6) passes all three jobs. Its archive
passes repeat SHA-256 comparison, installed source builds and resource isolation.
The downloaded archive also passes CPU execution and fifteen real GPU builds /
ninety physical executions on RTX 5090 and Intel ARL, loader-negative diagnostics
and all seven missing-owned-resource checks after Unicode/space relocation.
This baseline archive predates the separately pushed float64 fix 73ae38d6; it is
not evidence that the new numerical policy is in that older archive.

The first physical archive audit stopped because its read-only RPATH check
unnecessarily required patchelf. No system package was installed. Qualification
now reads both ELF RPATH/RUNPATH entries with the existing readelf; actual archive
creation still uses patchelf in hosted CI. Twelve real ELF fixtures verify safe
relative paths, both dynamic-tag forms, outside-prefix escapes, malformed ORIGIN
prefixes and empty search components. The physical archive acceptance above then
passes. A distribution CTest and hosted step retain this regression.

RuntimeBundles can attach an inventoried CPU-runtime-only archive to the GPU
host, retaining its private linkers and checking versions, all five platforms,
macOS 26 minimum and destination-tool/dynamic-library exclusion. Local composition
with actual published v0.99.1 CPU runtime data adds 1,714 files; all 508 pre-existing
host/GPU resource hashes remain unchanged. Repeating composition produces the
same archive digest. A v0.99.0 runtime archive is rejected without publishing any
result. This local composition check uses published runtime data, not a claim of
same-head cross qualification. New PR CI obtains runtime data from the exact head,
assembles twice, then reruns relocated combined-package checks before upload.

The user documentation now distinguishes experimental Vulkan availability,
required headers/loader, explicit optimized matmul and Intel float64 division
fallback. All 51 authored examples / 87 rendered blocks are checked on Mac Metal
and Linux Vulkan; all four GPU programs execute on each. The verifier selects a
backend and reports the other backend's shell commands as not run; each command
has actual matching-platform execution evidence. No Bun/tool installation was
needed on the Linux machine.

The locally composed combined archive also passes the full installed acceptance:
five CPU target records/inventories, CPU execution, fifteen GPU builds/ninety
physical executions, six output protections, both missing-loader diagnostics and
seven owned-resource failures. Its host/resource byte-preservation and repeated
archive digest checks pass. The new read-only ELF admission check passes through
root CTest as well. Exact current-head combined-package CI remains mandatory;
these local checks do not substitute older runtime data for that gate.


### Windows runtime object qualification

The first native Windows x86-64 and ARM64 runs (41287276948) compile six runtime
translation units, then expose a genuine namespace-lookup portability error in
Language.h: an unqualified friend Runtime resolves to the outer raw runtime under
MSVC lookup rules. Explicit forward declarations in the language namespace bind
the intended runtime and numeric executable without relaxing private access or
disabling warnings. Linux rebuild and all fourteen standalone tests pass after
the correction. Both rejected Windows logs are retained; Windows success remains
pending the corrected run. These are build/admission checks, not device execution.

The user-facing arithmetic description also states the existing fallback
boundary precisely: the selected companion replaces float64 arithmetic for a
whole kernel, retaining native float32. Kernels without unsupported division
remain native; per-instruction fallback selection is not claimed.


### Full public source parity and resident identity repair

The full public capability matrix has 66 supported source fixtures. All run on
physical Metal under explicit software arithmetic at workers 1/2/4. Comparing
those outputs through the complete Vulkan compiler finds a real omission:
matrix-elementwise.k's resident identity helper is rejected with G001 because
the generic numerical builder had no device stage. The existing classic Metal
path accepts it. The failed compilation is retained in shared-capabilities-build.log.

The shared builder now materializes that resident result using its normal map
copy stage only when the program would otherwise have no kernels. It retains
the original expression/type, ordinary output allocation, capture ownership,
shape checks, quota and cancellation path. No fake arithmetic is inserted and
there is no precision policy relaxation. Metal frontend diagnostic and original
artifact checks pass after rebuilding the shared compiler.

All 66 fixtures now match the Metal reference on explicitly selected NVIDIA and
Intel ICDs at workers 1/2/4: 594 primary comparison executions across three
backends. Initial/default-device replays remain separate controls. The comparison
uses software arithmetic and supplements independent numerical proofs.
Twenty additional complete identity tasks (hardware/software, all ten types)
pass 120 source executions. Independent raw copy tests pass 202,792 bit patterns
per policy/device, 811,168 total, without native Float64 enabled on the VkDevice.
This checks signaling/quiet NaN payloads, signed zeros, subnormals, empty buffers,
packed tails, unchanged inputs, ownership retirement and zero validation errors.
Hosted source compilation now retains both the full capability and identity gates.

The corrected 88a9f58c Windows runs pass on both x86-64 and ARM64: all seven actual
runtime objects have the expected COFF machine; native reflection/path/loader
checks pass, the adjacent DLL decoy is ignored and no Vulkan PE import is present.
These records explicitly report gpuExecution=false and installedCompiler=false.
