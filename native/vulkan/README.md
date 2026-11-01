# Experimental Vulkan runtime bring-up

This is an internal backend implementation, not a released Vulkan compiler target.
The source-to-device compiler and structured task integration are still pending:
see [implementation plan](../../docs/native-vulkan-roadmap.md) and #263.

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

The two built kernels are unmodified upstream `add.comp` and `mul_mm.comp` from
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
before source-language integration.

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

This initial path uses host-visible storage on discrete GPUs, synchronous fence
waits and an absolute build-tree shader directory. It is not a performance or
relocatable-distribution implementation. Device-local staging, async task
completion/cancellation/deadlines in the source supervisor, full type/rank support,
capability dispatch, fixed-tree policy, user-facing diagnostics and packaging
remain required before the backend can be considered ready. The explicit
right-transposed matmul interface is a private layout contract, not new language
syntax. Empty outputs and zero-contraction products perform no GPU dispatch.

See the dated qualification entry in the implementation plan for actual tested
hardware and unresolved sanitizer findings. Do not use wall-clock test duration
as a kernel benchmark, and coordinate performance runs with the other GPU user.

## Compiler artifacts and managed dispatch

The shared `gpu-source` library retains the existing typed map/program emitter.
Private `kalandra-vulkan-kernels`, `kalandra-vulkan-lower` and
`kalandra-vulkan-extract` tools produce compiler-owned storage-buffer SPIR-V;
this is not yet a user-facing source-to-executable backend. All ten numeric
types have source-derived map-chain conformance on both NVIDIA and Intel,
including checked integer overflow, packed narrow tails and empty domains.
The Vulkan lowering preserves native float64 where supported; explicit software
mode uses the existing integer IEEE implementation. Automatic fallback artifact
selection is still pending. Hardware float32 follows the existing hardware FTZ
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
program/transfer integration and sanitizer qualification remain unfinished.

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
repository. Transform/fallback artifact support and release packaging are still
unfinished; see the tracked completion boundary before making support claims.

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
