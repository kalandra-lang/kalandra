# Kalandra

A native language for CPU and GPU computing. [kalandra.dev](https://kalandra.dev).

Kalandra's native implementation uses C++23 and MLIR/LLVM. It combines immutable values, automatic memory management, structured parallel tasks and explicit GPU execution. Current development targets Apple Silicon with Metal.

```kalandra
fn double(value: int) -> int:
  value * 2

fn main() -> int ! TaskError:
  try parallel:
    try await spawn double(21)
```

Native source files use `.k`. The compiler is under development; the [native implementation guide](native/README.md), [language design](docs/design/core.md) and [roadmap](docs/roadmap.md) describe supported features and remaining work.

The native toolchain version is **0.99.0**. Run `kalandra --version` to identify an installed compiler. [Release notes](docs/releases/0.99.0.md) explain the move from the 0.12.0 line to native CPU/GPU execution.

## Install

Download the toolchain and `kalandra-native.vsix` from [GitHub Releases](https://github.com/kalandra-lang/kalandra/releases/tag/v0.99.0). Choose the standalone Metal DMG for macOS 26+ on Apple silicon, or the standalone CPU archive for Linux or Windows x86-64. Verify the accompanying SHA-256 checksum and keep the complete toolchain directory together.

Follow the [installation guide](https://kalandra.dev/docs/installation) for platform prerequisites and PATH setup. The [editor guide](https://kalandra.dev/docs/tooling) covers VS Code's **Install from VSIX** command and other LSP editors. Native tools and the installed editor client require no JavaScript runtime.

## Build from source

Install the matching LLVM/MLIR 23.1.3 toolchain, toml++ 3.4.0, CMake and Ninja described in the [build guide](native/README.md#build-and-test), then run:

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

Compile and run a native program:

```sh
build/native/native/kalandra main.k -o program
./program
```

The native toolchain does not require Bun or Node. Local CPU execution and Apple Metal GPU execution are implemented; [distributed execution](docs/native-distributed-roadmap.md) follows a separate roadmap.

## Website and documentation

Start with the [tutorial](https://kalandra.dev/docs/tutorial), [language reference](https://kalandra.dev/docs/language-reference), [built-in function index](https://kalandra.dev/docs/library) or [contributor guide](https://kalandra.dev/docs/contributing). Repository design/planning documentation is indexed in [docs/README.md](docs/README.md).

The website lives in `apps/website`. Its native language documentation is authored in Tiramisu under `apps/website/src/content/native`.

```sh
bun install --frozen-lockfile
bun run dev
bun run check
```

The root Bun workspace contains website development tools. `bun run build` builds the static site; the CMake commands above build and test the native compiler. The native VS Code extension has its own tooling in `native/editor/vscode`.

After building the native tools, verify the documentation with `bun run verify:docs`. This extracts the actual Tiramisu blocks, compares them with the rendered site, and checks expected diagnostics, CPU results, multi-file projects and local Git dependency workflows. `--tools /path/to/bin` selects another native tool directory. On a supported Mac with a Metal-enabled build, run `bun run verify:docs -- --gpu` to compile and execute the GPU examples too. Machine-provisioning recipes and the Windows-only PowerShell invocation are reported separately from executed programs. Linux qualification runs the CPU verifier in CI.

## Contributors and supporters

Thank you to everyone who contributed code, feedback, documentation, and financial support to Clio and helped keep these ideas alive. That work is part of Kalandra's history.

[See all contributors](https://github.com/kalandra-lang/kalandra/graphs/contributors) · [Contributing guide](CONTRIBUTING.md)

Support ongoing development through [Kalandra on Open Collective](https://opencollective.com/kalandra). Historical acknowledgements, including Ivo Balbaert, are preserved in [backers.md](backers.md).

[![Kalandra backers](https://opencollective.com/kalandra/backers.svg?width=890)](https://opencollective.com/kalandra)

## Origins and license

Kalandra continues the ideas of [Clio](https://clio-lang.org). Its implementation starts fresh here. The website scaffold comes from [Tiramisu Docs](https://github.com/pouya-eghbali/tiramisu-docs).

Apache-2.0. See `LICENSE` and `NOTICE`.
