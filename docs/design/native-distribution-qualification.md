# Native distribution and platform qualification

N5 is **complete for the agreed scope**: macOS ARM64 with CPU/Metal, plus CPU
qualification on Linux x86-64 and Windows x86-64. FreeBSD is deferred by the
publisher. The supported macOS minimum is **26.0**, selected by the publisher.
Hosted qualification now uses `macos-26`; CPU and Metal application compilation
explicitly preserve that deployment target. CUDA remains a separate roadmap.

## Distribution contract

The macOS workflow builds external-toolchain and standalone variants of the
editor tools, CPU compiler, and mixed CPU/Metal compiler. The mixed compiler is
the primary macOS language distribution. Its private tools include LLVM/MLIR,
Clang and compiler resources, SPIRV-Cross, SPIRV-Tools, and strict numerical shader
helpers. Every included dependency has an accompanying license and inventory.
Apple's SDK, linker and exact Metal compiler component remain Apple platform
prerequisites; Apple's compiler is not redistributed. The installed driver
checks its Apple signature and pinned version, **32023.864**, before use.

Keep the extracted prefix together. Native tools resolve private dependencies
relative to their installation, including symlink invocation. Missing private
resources produce an error; installed compilers cannot fall back to development
paths. Ordinary generated applications execute without the compiler distribution.
Sanitized applications require the corresponding compiler sanitizer runtime.

macOS dependencies use audited `@loader_path` references with no residual build
RPATHs. Linux packaging recursively vendors approved ELF dependencies and audits
relative `$ORIGIN` search paths. glibc, libstdc++ and libgcc_s remain declared
Linux system runtimes. Windows packaging audits PE imports and places private
LLVM DLLs beside their loaders. Microsoft C++ Build Tools and the Windows SDK
remain Windows compilation prerequisites. Git is required for dependency
installation on each platform.

Windows does not support LLVM ThreadSanitizer. The compiler explicitly rejects
`--thread-sanitize` before replacing an output. Normal, optimized and ASan/UBSan
modes remain required Windows execution gates. This limitation does not change
language types or permit silently uninstrumented sanitizer builds.

## Reproducibility and isolation

Every archive has a SHA-256 sidecar and extracted `SHA256SUMS` inventory. GNU
tar/gzip timestamps, ownership, order and modes are normalized. `SOURCE_DATE_EPOCH`
defaults to zero. Qualification independently compiles the source into two
build directories and requires byte-identical standalone archives: editor, CPU
and mixed Metal on macOS; CPU/editor on Linux and Windows. LLVM's deterministic archiver/indexer and source/debug prefix
mapping are part of that build contract. Windows COFF object generation disables
incremental-linker timestamps; PE linking uses reproducible hashes. Signing timestamps and notarization
receipts are separate from the reproducible unsigned inputs.

macOS acceptance relocates downloaded archives into paths with spaces and
Unicode. Native processes run under a sandbox denying the checkout, build tree,
Homebrew and network. Tests verify inventory hashes, signatures, deployment
metadata and every dynamic load. Strict and unfinished-code language-server
protocol suites execute through the sandbox. CPU tests cover all ten numeric
types, Bool/String and collections, workers, three runtime variants, symlink
invocation, missing resources, output preservation and compiler-independent
applications. Metal tests compile all ten numeric types with rank-eight maps,
resident reductions, selected stages, binary64 conversion and CPU workers in
normal, optimized, ASan/UBSan and TSan modes. Hosted tests compile GPU programs;
the same downloaded package executes them on the publisher's physical Mac.

Linux qualification uses a separate downloaded-artifact job without installing
LLVM or building the product. Bubblewrap denies the checkout and network while
allowing the relocated package and system platform tools. Portable CPU acceptance
requires execution of numeric scalars, arrays, tensors, owned records/enums,
Unicode strings, repeated worker awaits and expected errors, with worker counts
1/2/4. It also checks starter projects, pinned local package activation,
frozen/offline reuse, failed-build output preservation and execution after the
compiler is removed. Windows uses the same portable execution contract with its
explicit sanitizer limitation.

## Qualification evidence

The [machine-readable qualification record](native-platform-qualification-2026-10-23.json)
binds the successful hosted runs to source commit
`6f896e7361d41400f0bdee5ffa3542e2a275bd0a`, archive hashes, accepted Apple
receipts and exact signed-artifact execution checks.

- [macOS run 39973422436](https://github.com/kalandra-lang/kalandra/actions/runs/39973422436)
  passed independent byte-identical editor/CPU/Metal rebuilds, all seven installed
  acceptance tests, fresh downloaded-package tests, Developer ID signing,
  accepted notarization, stapling and signed-image Gatekeeper assessment on macOS 26. The exact downloaded signed DMGs passed quarantine checks locally without
  re-signing or removing quarantine. Their mixed compiler executed all ten numeric
  types, rank-eight maps and CPU worker bridges in normal, optimized, ASan/UBSan
  and TSan modes, plus the resident matrix graph and compiler-independent moved
  applications, on the publisher's macOS 27.2 Apple M2 Max.
- [Linux run 39973422726](https://github.com/kalandra-lang/kalandra/actions/runs/39973422726)
  passed ELF dependency auditing, byte-identical independent builds and 263
  successful CPU/project compiles in a separate downloaded-artifact job. All ten
  numeric types, rank-zero/rank-eight tensors, ownership/errors, Unicode paths,
  worker counts 1/2/4, all four host modes, offline/cache/resource rejection,
  compiler independence and both LSP suites passed. Bubblewrap denied the checkout
  and network; the acceptance job installed no LLVM and built no product.
- [Windows run 39973422486](https://github.com/kalandra-lang/kalandra/actions/runs/39973422486)
  passed PE import auditing, byte-identical independent builds, 198 successful
  CPU/project compiles, offline/cache/resource rejection, compiler independence
  and strict/unfinished-code LSP protocols on Windows Server 2025 build 26100.
  Normal, optimized and ASan/UBSan modes passed with worker counts 1/2/4.
  Unsupported ThreadSanitizer requests explicitly reject before replacing output.
  The [SDK preflight](https://github.com/kalandra-lang/kalandra/actions/runs/39953666203)
  also passed: matching verified MLIR source supplies the development libraries
  omitted from the official Windows LLVM archive.
- The [general check](https://github.com/kalandra-lang/kalandra/actions/runs/39973422540)
  passed on the same source commit. Release helper tests passed all 22 cases;
  deterministic archive tests passed all three cases.

Physical GPU execution was qualified on macOS 27.2. macOS 26 is the hosted
build, CPU execution and shader-compilation baseline; every included Mach-O's
measured deployment requirement is checked. The per-command publication from
Metal commit to its uninstrumented completion callback is explicitly modeled for
TSan, and the signed GPU suite passes without sanitizer suppressions. Signed disk
images are Gatekeeper-assessed as images; the raw CLI assessment's expected
“not an app” diagnostic is retained alongside successful quarantined execution.

Branch rehearsals upload workflow artifacts and publish no release. The
[publisher signing setup](native-ci-signing.md) runs entirely on GitHub Actions.
Version-tag publication must remain gated on successful distribution checks.
N6 covers website, onboarding and release preparation. The historical
[local distribution record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-standalone-distribution-2026-10-23.json)
is retained as historical evidence, not substituted for current hosted results.
