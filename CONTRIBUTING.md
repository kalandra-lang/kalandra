# Contributing to Kalandra

Current development targets the native C++23 implementation. Start with the [native build guide](native/README.md), [language design](docs/design/core.md) and [roadmap](docs/roadmap.md). Native source examples use `.k`.

Use the pinned LLVM/MLIR/Clang toolchain with CMake and Ninja:

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

Keep lexer/parser, semantic analysis, typed IR, lowering and runtime responsibilities separate. Preserve source locations, immutable value semantics, checked failures and ownership through worker and device boundaries. Do not expose internal allocation or ABI details as language features without an agreed design.

Add tests for meaningful behavior changes and regressions. Use relevant debug, optimized and sanitizer configurations, and document the scope of qualification honestly. GPU tests need access to the target hardware. The numerical contract applies to every backend.

Keep native design documentation current with the implementation. Write user-facing documentation in Tiramisu under `apps/website/src/content/native`, using examples checked by the native compiler. Organize published pages as tutorials, language reference, built-in reference, HOWTOs, tool usage or contributor guidance. Keep organization planning, qualification reports and experiment notes in repository documentation, not published lessons. Every built-in needs a signature, behavior, failure/placement rules and an entry in `tests/fixtures/public-api.json`.

Run `bun run check` for website and documentation changes. Run `bun run verify:docs --gpu` on a supported Mac with Metal tools to execute the authored examples; omit `--gpu` for CPU-only verification, which reports the GPU execution gap explicitly. Register expected example outputs in `tests/fixtures/docs-examples.json`. The website uses Bun independently of the native CMake build.

Submit focused changes with a clear explanation of behavior, validation and remaining limitations. Please retain project history and contributor attribution.
