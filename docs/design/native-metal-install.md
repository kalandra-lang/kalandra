# Installing the native Apple Metal compiler

The `native-metal-package` target adds Apple Metal compilation to the relocatable
[CPU compiler bundle](native-cpu-install.md). Configure both
`KALANDRA_BUILD_METAL_RUNTIME=ON` and `KALANDRA_BUILD_METAL_PROOF=ON`, using the
pinned [Apple GPU toolchain](apple-gpu.md), then build:

```sh
cmake --build build/native --target native-metal-package
```

Extract `build/native/native-metal/kalandra-native-metal-macos-arm64.tar.gz`
and add its `bin` directory to PATH. CPU compilation remains available; add
`--target metal` for GPU programs:

```sh
kalandra main.k --target metal -O2 -o app
./app/program
kalandra --project path/to/kalandra.toml --target metal -o app
```

Keep the installed prefix together. `kalandra` resolves its compiler-private
shader tools, host headers and CPU/Metal runtime archives relative to its actual
executable, including a symlink invocation. Missing installed resources never
fall back to source/build-tree copies. The Metal compiler is separate from the
CPU-only installed executable, so CPU packages retain their existing contract.

The archive includes all CPU/editor resources, four Metal runtime variants, the
optimized task-core archive, strict-lowering and extraction executables, private
generated-host headers and Metal dependency metadata. These are compiler resources;
they do not establish the separately planned public C/C++ interoperability ABI.
For direct installation, install `NativeEditorTools`, `NativeCPUCompiler` and then
`NativeMetalCompiler` to the same prefix. The final component replaces `kalandra`
with the CPU/Metal-capable driver. Archive layout requires `bin`, `lib` and `share`.

LLVM/MLIR/Clang 23.1.3, SPIRV-Cross, SPIRV-Tools, Apple's selected Metal compiler
and the macOS SDK remain external. Exact SPIR-V roots, verified Metal compiler path, SDK path and Metal toolchain identity
are recorded under `share/kalandra-native-metal/toolchain.json`. This bundle does
not relocate those dependencies, bundle Apple's compiler, sign/notarize its tools,
or qualify another platform. The native release workflow also builds a standalone
variant, includes the mixed compiler in automated publication, and signs/notarizes
its standalone DMG on GitHub Actions. See the standalone distribution below.

A generated Metal application is a directory containing `program` and its shader
libraries. Keep those files together when moving the application. Ordinary
applications do not need the Kalandra compiler bundle or LLVM/MLIR to run, but do
require compatible Apple Metal hardware and system frameworks. ASan/UBSan and TSan
host modes additionally require platform sanitizer runtimes.

## Installed numerical libraries

The Metal-capable compiler consumes the same Git dependencies and `kalandra.lock`
as CPU builds. Install explicitly, then compile from the verified cache:

```sh
kalandra install --project path/to/kalandra.toml
kalandra --project path/to/kalandra.toml --target metal -O2 -o app
./app/program
```

To restore a checkout from its existing lock, use `install --frozen`. With all
snapshots already available, `install --frozen --offline` verifies the cache
without running Git. Compilation never installs dependencies. Move the complete
project and its `.kalandra` cache together for offline compilation; a fresh
checkout needs an explicit install first.

To refresh one managed root, use the [named update command](native-package-update.md):

```sh
kalandra update numerical --project path/to/kalandra.toml
```

The lock must match the manifest. Recheck and rebuild after an update. Already
compiled applications keep their compiled code; changing the lock does not
change their behavior.

Each library retains its private dependency versions in CPU workers and GPU
helpers. Relative imports and re-exports retain that owner too. The same eligible
numerical function can run in a CPU worker or be dispatched on a GPU; importing it
does not broaden the [supported GPU type/operation subset](native-gpu-capabilities.md). Installed sources and
the public lock remain protected from build output. An altered snapshot prevents
compilation before an existing application is replaced.

Once compiled, generated applications do not load source packages or lockfiles.
Their `program` and shader libraries can be moved together and run without the
project cache, original repositories or compiler bundle. Platform/framework and
sanitizer-runtime requirements above still apply.

## Acceptance

After building the target, run:

```sh
ctest --test-dir build/native -R '^native-metal-install$' --output-on-failure
```

This test stages and packages the components, verifies complete checksums and
metadata, extracts into a prefix with spaces and deletes the original stage.
From a working directory outside the checkout, a compiler symlink builds CPU
source and GPU source in normal, optimized, ASan/UBSan and TSan modes. Explicit
project/package imports are checked too. Removing a private shader tool produces
a resource error and preserves the existing application executable. Finally the
test removes the installed compiler bundle, moves all generated applications and
executes them again. The independent CPU/editor installation gates remain in place.

The installed-package acceptance creates a numerical library and a wrapper that
privately depends on its `main` branch. Both the application and wrapper initially
resolve that branch to the same commit. Across all four host modes, a CPU worker
calls the wrapper and GPU calls combine the direct and private versions, producing
exact float32 bits for `[6, 8, 10]`.

After the numerical branch moves, normal installation preserves the old lock.
`update numerical` changes only the application's root selection; the unchanged
wrapper retains its private older commit. A later broken revision fails to
activate and leaves the lock and previously generated applications unchanged.
The test restores the updated lock into a fresh cache with `--frozen`, removes
both original repositories, relocates the project/cache, then verifies
`--frozen --offline` and placement diagnostics. Rebuilt applications return
`[105, 107, 109]` in all four host modes, demonstrating the distinct direct/private
versions on CPU and GPU.

Builds preserve source/lock/cache bytes, reject snapshot corruption without
replacing an existing application and reject outputs inside the cache or at the
public lock. Both pre-update and post-update applications run again after deleting
their project/cache, and after deleting the compiler and moving the applications.

The [named-update Metal qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-metal-package-update-2026-10-21.json)
records the installed Debug/Release compiler runs and their exact checked results.

The [installed-package qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-metal-packages-2026-10-20.json)
records the tested compiler configurations and hardware. These checks qualify
this scoped numerical CPU/Metal path, not new GPU language features, another
platform, authenticated transport, or hosted Metal release automation. ASan/UBSan
and TSan instrument generated host code; they are not shader sanitizers.

## Typed numerical programs

The installed compiler includes the typed graph compiler and strict64 shader
lowering resource. Relocation acceptance builds all ten numeric types through
rank-eight maps, resident sums/feedback, selected numerical stages, conversion
to binary64 and CPU workers, in all four host modes. The applications run again
after deleting the compiler bundle and moving their directories. Missing
strict64 resources reject compilation while preserving a previous application.
See the [typed program contract](native-gpu-typed-programs.md).

## Standalone hosted distribution

The primary macOS distribution now bundles the private LLVM/MLIR, Clang,
SPIRV-Cross/SPIRV-Tools and strict numerical helper closure. It builds, signs and
notarizes entirely on GitHub Actions. Apple SDK/linker and the exact pinned Apple
Metal compiler remain platform prerequisites. The supported minimum is macOS
26.0; generated CPU executables and Metal shaders preserve that deployment target.
Hosted downloaded-package compilation and physical GPU execution are independent
gates. See the [distribution checklist](native-distribution-qualification.md).
