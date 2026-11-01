# Experimental Vulkan runtime bring-up

This is an experimental opt-in backend, not a released Vulkan compiler package.
The development compiler and a production-pinned Linux archive candidate accept
`--target vulkan`. The archive passes physical relocation checks; combined CPU
target-runtime packages and the full platform matrix still require qualification. See [implementation plan](../../docs/native-vulkan-roadmap.md) and #263.

With an already configured root build using `KALANDRA_BUILD_VULKAN_RUNTIME=ON`:

```sh
cmake --build build/native --target kalandra --parallel 2
build/native/native/kalandra --target vulkan native/metal-source/placed.k -O2 -o build/vulkan-hello
build/vulkan-hello/program
```

The output is a directory containing the executable and its generated SPIR-V.
Project manifests and local packages use the common `--project`/`--package`
options. All three arithmetic policies and `-O0`/`-O2` are supported. Normal
Linux root builds also support `--sanitize` with separate ASan/UBSan host/runtime
libraries and instrumented CPU exports; this does not instrument GPU instructions.
NVIDIA exit-time LeakSanitizer findings remain open. `--thread-sanitize` is still
rejected; private Intel TSan qualification is a separate configuration. Private
instrumented root profiles expose the proof tools rather than the public Vulkan
command. Matmul defaults to `fixed-tree`; explicit `--matmul-policy optimized`
routes float32 source products through the imported tiled kernel. Software
arithmetic retains fixed-tree products. The build
uses configured host C++/LLVM/SPIR-V tools and host development headers. The
Linux archive candidate provides private compiler tools; GPU C++ compilation
still needs system development headers. Cross-target GPU compilation remains
unsupported. No system dependencies are installed by the compiler.

The opt-in root build flag is `KALANDRA_BUILD_VULKAN_RUNTIME`. To qualify just the
runtime with existing Vulkan development tools, without installing LLVM/MLIR:

```sh
cmake -S native/vulkan -B build/vulkan -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/vulkan --parallel 2
ctest --test-dir build/vulkan --output-on-failure
# Test another enumerated hardware GPU explicitly:
build/vulkan/kalandra-vulkan-tests 1
```

Requires Vulkan headers/loader, glslc, spirv-val, spirv-dis, CMake 3.28+, a C++23
compiler/standard library, Python 3 for the provenance audit, and the Khronos
validation layer for hardware tests. No dependency downloads or installation
commands run during configure/build. System software on the user's Linux host
must be installed only by the user or with their explicit approval.

## Kernel provenance and numerical scope

The two primitive kernels are unmodified upstream `add.comp` and `mul_mm.comp` from
llama.cpp revision `a91eab67d8389d979d29fdf6633b2edd0718eb4b` (MIT). Their
transitive textual includes are retained unmodified. [UPSTREAM.json](third_party/ggml/UPSTREAM.json)
records SHA-256 hashes and [LICENSE](third_party/ggml/LICENSE) retains notices.
The imported source set contains conditional quantization, expert-routing and
cooperative-matrix code, but these variants are not enabled or claimed supported.
No Steel/CUTLASS/CuTe code or unrelated shader library is imported here.

Build definitions select float32 inputs, outputs and accumulators; the tiled
matrix kernel uses ordinary float32 arithmetic, not cooperative-matrix TF32/FP16.
Both actual SPIR-V outputs must validate and contain only the Shader capability
and 32-bit floating-point types. A runtime one-product case detects reduced
operand precision. Matrix tests compare every output against independent double
references with a magnitude-based float32 error bound. This does not establish
bitwise equivalence with Metal's historical fixed-tree policy, nor qualification
of NaN/denormal/signed-zero behavior. Those policies must be handled explicitly
when selecting the optimized source policy.

## Ownership and bring-up limits

Buffers own their allocation and retain the device independently of the runtime
facade. Upload copies host data; inputs remain immutable; foreign contexts and
invalid shapes are rejected. The quota covers actual live Vulkan allocation
sizes, including alignment. Queue submissions are serialized per runtime, with one command
in flight; fences settle submitted access before descriptor/command/buffer
resources are released. Noncoherent mappings are flushed/invalidated explicitly.
Published immutable buffers can be read while unrelated queue work runs; cache
invalidation is synchronized per allocation. Fresh uploads and kernel creation
also avoid holding the queue's fence-wait mutex.
Software devices are excluded rather than used as a silent GPU fallback.

This path uses host-visible storage on discrete GPUs and synchronous raw fence
waits. Managed tasks put those waits on a foreign completion service. All 14
predefined shaders are embedded in the runtime archive; raw primitive execution
does not read build-tree shader files. Addition and tiled matrix pipelines are
created on first nonempty use, with their own workgroup/shared-memory limits.
Creating a context for a basic map does not require the matrix kernel's tile
resources. The data-only resource object has no Vulkan-loader dependency.
Device-local staging, user-facing diagnostics and installed compiler packaging
remain unfinished. The explicit
right-transposed matmul interface is a private layout contract, not new language
syntax. Empty outputs and zero-contraction products perform no GPU dispatch.

See the dated qualification entry in the implementation plan for actual tested
hardware and unresolved sanitizer findings. Do not use wall-clock test duration
as a kernel benchmark, and coordinate performance runs with the other GPU user.

## Compiler artifacts and managed dispatch

The shared `gpu-source` library retains the existing typed map/program emitter.
Private `kalandra-vulkan-kernels`, `kalandra-vulkan-lower` and
`kalandra-vulkan-extract` tools produce compiler-owned storage-buffer SPIR-V;
the development `--target vulkan` command drives those stages in C++. All ten numeric
types have source-derived map-chain conformance on both NVIDIA and Intel,
including checked integer overflow, packed narrow tails and empty domains.
The Vulkan lowering preserves native float64 where supported; explicit software
mode uses the existing integer IEEE implementation. At load time native float64
requests signed-zero/infinity/NaN preservation and, when advertised, denormal
preservation. These controls do not substitute software arithmetic or enable
fast math. Intel native float64 division currently fails the independent edge
corpus even with those controls, so it reports Unsupported (106). Use explicit
`hardware-with-software-fallback` for that operation on Intel; other float64
operations remain native. NVIDIA RTX 5090 passes the same native division corpus.
This is a conservative Intel capability restriction, not a claim that every
Intel driver has been tested. No fallback is selected in `hardware` mode. Explicit native-first software-fallback
bundles are qualified below. Hardware float32 follows the existing hardware FTZ
contract; software tests use a separate gradual-underflow oracle.

Versioned `_vk_flat_v1` entry points flatten a 2D physical grid into the shared
linear tensor domain. Dispatches can exceed 65,535 groups; padded lanes retain
the original bounds guard and uint32 index overflow is rejected before launch.
The large-tail test checks all 4,194,257 elements through both map stages.

`AsyncRuntime` integrates private dispatches with the existing task core through
external completion capabilities. Its bounded foreign completion thread waits
on GPU fences; scheduler workers do not. Captures copy parameter words and own
immutable buffers. Cancellation/deadlines retain resources through completion
acknowledgement; device errors remain native faults. Deterministic tests cover
held completion, cancellation/fault precedence, capture limits, caller/facade/core
teardown and cached output ownership at worker counts 1, 2 and 4. Full source
program/transfer integration is exercised by the complete-source checks below;
sanitizer findings remain open as documented below.

Sanitizer builds are separate: `KALANDRA_VULKAN_SANITIZERS=ON` enables ASan/UBSan;
`KALANDRA_VULKAN_THREAD_SANITIZER=ON` enables TSan. They cannot be combined in one
build directory. Default hardware tests enable Vulkan validation. Private
runtime/async tests accept `--no-validation` after the positional device index
for separate TSan investigation of third-party validation-layer reports. This
does not disable Kalandra instrumentation or change production defaults. Their
optional `--maps` and the independent loader control's `--maps-twice` capture
Linux loaded-module mappings before unload to investigate unsuppressed exit
leaks. See the roadmap for the retained failures and the exact qualified scope.

## Linux development toolchain 23.1.4

The production lock remains LLVM/Clang/MLIR 23.1.3. An opt-in Linux development
profile accepts the matching 23.1.4 toolchain for Vulkan qualification:
`-DKALANDRA_DEVELOPMENT_LLVM_VERSION=23.1.4`. It still requires exact agreement
between the C compiler, C++ compiler, LLVM and MLIR versions and disables CMake
installation rules. It does not certify or publish 23.1.4 release packages.

With the owner's explicit authorization, `qualification/provision_llvm.py`
downloads the official 23.1.4 development archive into a project-local directory,
verifies its pinned SHA-256 digest and extracts it there. No system package,
driver or compiler is installed or replaced. Configure the full project with
`-DKALANDRA_LLVM_ROOT=<verified archive root>` and explicitly keep the installed
compiler with `-DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++`.
The owner installed `llvm23-devel` and `tomlplusplus-devel`; no toml++ download
is needed. Provenance is retained in `qualification/toolchain-23.1.4.json`.

## Private source-supervisor qualification

`kalandra-vulkan-supervisor` now shares the existing typed host continuation
emitter with Metal. The private `qualification/source_program.py` driver emits
that supervisor and any CPU exports, compiles the numerical MLIR to validated
SPIR-V, and links the native Vulkan task/transfer runtime. It uses an existing
configured project build and installs nothing. This is not an installed
`--target vulkan` command or distribution qualification.

The source adapter supports explicit CPU/GPU transfers over retained immutable
CPU descriptors, typed results, prepared resident numerical programs and
selected-path source control. CPU exports require a downloaded host result;
there is no implicit numerical CPU fallback. Host allocations and device
allocations share a registry byte ledger, including full retained CPU allocation
sizes for subviews. Prepared code and captured invocation metadata share a
separate argument ledger; all device services share one pending-operation limit.
Cancellation retains captured backing through acknowledgement on both uploads
and downloads. The scheduler never waits on a transfer fence.

The same source-program fixtures used by Metal can select the private Vulkan
driver with `--backend vulkan`. Their independently checked expected-output
records can be replayed with `qualification/replay_programs.py` under an explicitly
selected physical Vulkan ICD. Generated artifacts/results remain outside the
repository. Transform/fallback artifacts are covered below; release CLI and
packaging remain unfinished. See the tracked completion boundary before making
support claims.

For the shared fixtures, use the checked-in private adapter with existing paths:

```sh
export KALANDRA_VULKAN_BUILD_ROOT=/absolute/project/build
export KALANDRA_VULKAN_LLVM_ROOT=/absolute/verified/llvm-root
python3 native/metal-source/NumericProgramTests.py \
  --compiler "$PWD/native/vulkan/qualification/compiler_driver.py" \
  --backend vulkan --arithmetic hardware --variant optimized \
  --root /absolute/qualification/results
```

Repeat with `--arithmetic software` and a separate result directory. The replay
helper checks the generated independent `expected.json` records; select the
installed physical ICD through `VK_DRIVER_FILES` when testing a second device.
The adapter preserves `-O0`/`-O2` and rejects unsupported target/policy arguments.

## Predefined tensor transforms

The private source pipeline supports bitwise rank-2 transpose for all ten numeric
types in hardware/software arithmetic. The tile body is adapted from the pinned
MIT ggml `copy_transpose.comp`; its two uint32 planes preserve every 64-bit cell,
including NaN payloads and signed zeros. No float64 arithmetic is needed just to
transpose float64 storage. Rounded tile tails and two-dimensional group grids
are checked against logical bounds.

Native float32 `softmaxRows` and `rmsNormRows` reuse Kalandra's owned Metal row
algorithm, preserving its adjacent-pair tree, NaN/+infinity handling, host or
device epsilon, and invalid-epsilon fault before empty-output handling. They
require Vulkan 1.2 and advertised float32 SignedZeroInfNanPreserve. Missing
capability is Unsupported (106); no CPU or software fallback is hidden. Software
row arithmetic and float64 row arithmetic remain explicitly rejected, as on Metal.

Hardware precision is device-specific. Vulkan's native division, exp and sqrt
need not match CPU/Metal bits. Qualification propagates the normative SPIR-V
bounds through the same reduction tree: division 2.5 ULP in its guaranteed
range, exp `(3+2*abs(x))` ULP, sqrt inherited from reciprocal of a 2-ULP inverse
sqrt. Basic operations use Vulkan's implementation-defined rounding; contraction
and reassociation are disabled. Special-value handling and transpose payloads
have separate stronger checks. See the [Khronos precision specification](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
The unchanged shared CPU/Metal Decimal oracle is retained separately. The Vulkan
profile is selected explicitly with `transform_oracles.py --profile vulkan-hardware`.

An exact Metal RMS oracle rejected NVIDIA's `[-3]` case by one ULP. Intermediate
readback isolated native sqrt; integer midpoint correction exposed approximate
division as well and was rejected. Explicit SPIR-V round-to-nearest then triggered
SIGILL inside NVIDIA 595.112.07's GPU compiler. The separately tested
NaN/infinity-preservation-only artifact works on NVIDIA and Intel. Neither the
rounding-mode artifact nor the correction algorithm is selected by production
code. These failures are retained in qualification evidence; they are not
reported as exact-bit passes.

The private `qualification/source_transforms.py` compiles 24 complete programs,
including repeated awaits, device-computed epsilon and arithmetic faults. It runs
both physical ICDs at worker counts 1/2/4; `--compile-only` lets hosted CI verify
artifacts without substituting a software Vulkan implementation. Managed tests
hold acknowledgement to check cancellation/deadline/fault precedence, scratch
quota rollback and resource retirement.

The source registry opens additional physical GPU contexts on selection rather
than during enumeration. Concurrent selection publishes a single immutable
context/service pair and preserves shared quotas. The default service is still
created at startup for CPU imports and structured error tasks; failure of that
selected default remains a startup error. Private stats expose the initialized
context count for qualification.


## Explicit arithmetic fallback bundles

`hardware-with-software-fallback` emits the native SPIR-V plus an ABI-compatible
GPU integer float64 variant. The generated host enables fallback only for that
explicit policy. Loading tries native first; only Unsupported (106) can select
the bundled alternative. A missing native artifact, malformed module or device
fault is not a fallback trigger. A missing alternative retains Unsupported;
a malformed alternative remains an artifact error. No CPU numerical fallback.

The fallback variant preserves supported native float32 instructions and
comparisons. An initial all-width lowering attempt failed the shared graph
suite at a float32 comparison and was rejected. The selective float64 pass has
an independent mixed-width regression; default CPU/Metal/software lowering is
unchanged, and unsupported widths still fail closed. Plain `software` remains
an explicitly selected implementation for both floating widths.

A private capability ceiling disables float64 in VkDeviceCreateInfo as well as
admission, so tests cannot accidentally execute native float64. Under that
ceiling, hardware-only source programs reject with 106; explicit software and
fallback programs execute on the physical GPU. Native-enabled controls verify
native is preferred. The private source builder keeps the original generated
host and records the ceiling in a separate test host; this is not a released
compiler flag.

Qualification covers 67 graph cases on both GPUs at workers 1/2/4 (402 source
executions), plus twelve add/subtract/multiply/divide policy variants (72 source
executions, including 24 expected hardware capability rejections). Independent
CPU hardware checks verify 6,032 software IEEE arithmetic results per GPU,
including gradual underflow, signed zeros, overflow and canonical arithmetic NaNs.
Artifact-error tests verify the fallback trigger rules above. GCC and the
isolated Intel TSan configuration pass; CPU/Metal policy regressions pass.
ASan/UBSan assertions pass, but LeakSanitizer still exits unsuccessfully:
28,563 bytes / 77 allocations after repeated registry creation. Eleven direct
Vulkan-loader initializations with no Kalandra runtime reproduce that exact total.
This strengthens external-loader attribution without suppressing or relabelling
the failing leak check as clean.

All ten native/fallback map, reduction and matrix bundles compile and validate;
the reduction/matrix overflow, empty-domain and tail conformance checks also pass
on both physical GPUs with native float64 disabled.

### Native float64 edge qualification

The same independent 1,508-pair CPU IEEE corpus now checks each native operation
as well as its integer companion. With preservation controls, NVIDIA passes all
6,032 native and 6,032 software cases. Intel passes 4,524 native add/sub/mul
cases and all 6,032 software cases; native division is explicitly rejected.
The native oracle permits NaN payload differences only, with exact signed-zero,
finite, infinity and subnormal results. Historical FTZ and scaling experiments
are retained separately and are not accepted by weakening that oracle.

Use the generated fallback-source fixtures to repeat the division check with
exactly one physical ICD selected externally:

```sh
build/native/vulkan/kalandra-vulkan-fallback-tests \
  artifacts/fallback-source-suite/div-hardware-with-software-fallback/kernel0.spv 0 div
```

For the Intel capability restriction, add `--expect-native-unsupported`. That
mode asserts hardware-only rejection and explicit fallback selection; it reports
zero native division cases rather than claiming native IEEE conformance.

## Native conversions and bitwise ReLU

The Vulkan source emitter enables driver-advertised float64 conversion
instructions where available. The shared Metal default retains its existing
integer path. Checked admission still precedes a native float-to-integer cast;
invalid inputs never reach the conversion instruction. Narrow integer casts use
canonical carriers, and checked branches return integer bits so the explicit
float64 fallback can remove every double conversion without leaving a double
transport requirement. Identity conversions preserve payloads exactly.

The independent LLVM APFloat/APInt oracle covers every one of the 100 pairs,
including exhaustive 8/16-bit input domains, all floating exponent fields,
boundaries and deterministic random inputs. Each policy/device run checks
2,008,620 cells and guards/immutability across 760 submissions: 12,051,720 cells
across three policies and both physical GPUs. Native runs each use the documented
NaN/FTZ allowance for 136 results; software and float64-fallback runs use none.
SPIR-V audits require real double conversion instructions in native artifacts,
no double types/capabilities in fallback artifacts, and retained native binary32
casts. These are driver/API-native checks, not a silicon-throughput claim.

Complete-source checks add all 100 pairs under native and fallback policy,
repeated awaits, rank-3 storage and explicit downloads. Invalid overflow and
uniform conversion before an empty output also retain ArithmeticFault order:
212 programs, 1,272 source executions at workers 1/2/4 on both GPUs. Another
30 source ReLU programs pass all policies with float64 disabled (180 executions).
Bitwise ReLU qualification checks 202,792 cells per policy/device, preserving
NaN payloads and clamping negative zero without needing float64 arithmetic.

Default float32 and float64 CPU/Metal proof-generator artifacts remain
byte-identical; Metal graph/policy/frontend regressions pass. ASan/UBSan numerical
assertions and isolated Intel TSan checks pass. LSan still fails unsuppressed:
2,497 bytes/7 allocations for conversions and 2,509 bytes/7 allocations for ReLU.
The latter differs from the single-loader control by 12 bytes; it is not reported
as an exact match or a leak-clean result.

## Explicit optimized source matrix products

`--matmul-policy optimized` selects the existing MIT ggml float32 kernel for
float32 source `matmul` operations. It retains ordinary float32 operands and
accumulators, permits fused multiply-add and a different reduction order, and
makes no bitwise fixed-tree promise. Other numeric types retain their existing
products. `--arithmetic software` always uses fixed-tree products.

The source adapter privately transposes the immutable right operand on the GPU
before invoking the right-transposed primitive. This costs a second dispatch
and one additional right-operand buffer, charged to the existing allocation
quota. Empty and singleton dimensions skip the layout dispatch. No allocation
limit is raised. A prepared matrix operation uses one managed task record and
checks cancellation after layout completion before submitting multiplication.
Cancellation retains submitted resources until the fence acknowledgement.

Artifacts include a `.spv.fixed-tree.spv` companion. If the GPU cannot admit the
tile/layout limits, it selects that native GPU algorithm; this does not constitute
software arithmetic fallback. Missing/corrupt primaries and corrupt companions
fail closed. Numerical fallback permission cannot disguise a missing algorithm
companion. Built-in plans share immutable objects without context ownership
cycles or one kernel-quota charge per alias.

Physical tests cover 32 shape/algorithm combinations, ten admission-policy
cases and twelve managed layout/success/cancellation/deadline/failure cases per
GPU on RTX 5090 and Intel ARL. Shapes include empty dimensions, tile tails,
singletons and K=4097. Outputs are checked independently in double precision
with a magnitude-based float32 error bound; a one-product probe checks full
operand precision. These are correctness results, not performance measurements.

Isolated sanitizer checks distinguish loader findings from execution: Intel's
matrix corpus and loader-only control exit cleanly; NVIDIA's corpus and
loader-only control both fail unsuppressed LSan at 2,509 bytes/7 allocations.
Public sanitized optimized-matmul programs retain exact results on both GPUs,
with clean Intel exits and the existing NVIDIA two-initialization loader leak
(5,082 bytes/14 allocations). Public TSan remains unavailable; the separate
Intel TSan profile is private qualification only.

## System Vulkan loader

The raw runtime and generated applications dynamically resolve the system loader
instead of importing its SDK library. A missing or incomplete loader returns
Unavailable (104), including a complete application's `Vulkan unavailable: 104`
diagnostic. All used core functions must resolve before any driver entry.
Linux uses `libvulkan.so.1`; the Windows implementation restricts `vulkan-1.dll`
to System32 and remains platform-unqualified. There is no macOS Vulkan claim.

One successful OS-loader handle remains process lifetime, matching the old
ordinary library import, so captures released during static shutdown retain valid
function pointers. Individual instances/devices/ICDs still retire normally.
Negative tests cover concurrent first use, missing/incomplete loaders and ELF
imports without needing a GPU. This removes the SDK loader link dependency from
applications; configured compiler tools/C++ development headers and installed
GPU drivers are still required. Installed one-download packages remain pending.


## Combined CPU target runtimes

The CPU runtime-bundle tool can attach independently inventoried runtime-only
packs to a GPU host package. The host executables, GPU archives, lowering tools
and SPIR-V resources remain byte-identical; only CPU-target metadata, the new
runtime-data subtree and the top-level inventory change. Destination compiler
executables, shared libraries and system SDK libraries are excluded.

```sh
python3 native/packaging/RuntimeBundles.py \
  --host-package kalandra-native-vulkan-standalone-linux-x86_64.tar.gz \
  --runtime-bundle kalandra-cpu-target-runtimes.tar.gz --output bundled
```

Both input archives require their SHA-256 sidecars and complete inventories.
The assembler requires matching release/LLVM versions, all five supported CPU
targets, macOS 26 minimum and the host's private cross linkers. It rejects stale
runtime packs rather than mixing versions. CPU destinations retain their usual
external development-library requirements; this does not cross-compile GPU code.
PR qualification waits for same-revision CPU runtime data, assembles twice and
rechecks the combined installed compiler. That gate remains pending until its
current-head workflow completes.
