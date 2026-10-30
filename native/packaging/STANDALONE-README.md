# Kalandra standalone native tools — macOS ARM64

Extract the archive into a directory you control and add its `bin` to PATH.
Keep the entire directory together when moving it. LLVM and its non-system
dynamic dependencies are included under `lib/kalandra/toolchain`; Homebrew and
the development checkout are not runtime requirements.

The CPU variant also includes `kalandra`, its private runtime archives and a
private Clang with compiler resources. Compiling programs still requires Apple's
command-line tools, SDK and system linker. The editor-only variant needs none of
those build tools to check/format source or run the language server. The VS Code
extension is distributed separately.

The Metal variant is the mixed CPU/GPU compiler. It also includes private MLIR
and SPIR-V tools, CPU/Metal runtimes and shader-lowering helpers. GPU compilation
requires Apple's Metal compiler version 32023.864 (Xcode 26.2's Metal Toolchain)
and the macOS SDK. These Apple platform tools are resolved on the installed
machine and checked before compilation; development-machine paths are not used.
Select the compatible Xcode through DEVELOPER_DIR or xcode-select and install
its component with `xcodebuild -downloadComponent MetalToolchain`. An explicit
`KALANDRA_METAL_COMPILER` path is also accepted after Apple-signature and exact
version checks. Use `kalandra main.k --target metal -o app`, then `./app/program`.
Keep generated application directories together. Ordinary generated programs
need no compiler bundle; sanitizer modes additionally require sanitizer runtimes.

Read `lib/kalandra/toolchain/standalone.json` for the actual minimum macOS version,
architecture, dependency inventory and signature status. The highest deployment
requirement among all bundled binaries determines the minimum OS. A package
built on a newer OS is not automatically compatible with older macOS releases.

Unsigned tar archives use ad-hoc Mach-O signatures after relocation. The separate
DMG distributions use Developer ID signatures, notarization and stapled tickets;
verify their accompanying notarization receipts. `SHA256SUMS` and the archive
checksum detect accidental changes; they do not alone authenticate a publisher.
Dependency licenses are in `share/licenses/kalandra-toolchain`.

No source checkout, Bun, Node or Python is needed to run the tools. This package
is an N5 distribution qualification artifact, not a claim of clean-machine or
broader platform qualification. Project: https://github.com/kalandra-lang/kalandra
