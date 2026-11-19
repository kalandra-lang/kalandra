# Contributing to Kalandra

Current development targets the native C++23 implementation. Start with the [native build guide](native/README.md), [language design](docs/design/core.md) and [roadmap](docs/roadmap.md). Native source examples use `.k`.

## Propose a change

Anyone can suggest a change by opening a [GitHub issue](https://github.com/kalandra-lang/kalandra/issues). Explain the problem and your idea in your own words. Early ideas, questions and pseudocode are welcome; a complete specification, implementation or sponsor is not required. The **Design proposal** issue template is optional, and blank issues remain welcome.

Issues are also where we develop and decide substantial design changes, including syntax, semantics, ownership and concurrency rules, GPU programming models and major public API contracts. There is no separate RFC submission process, numbered-document requirement or mandatory waiting period. Bug fixes, routine library additions, documentation improvements and optimizations that preserve behavior can use ordinary pull requests.

Contributors and maintainers work out the proposal together in the issue. Before accepting a substantial change, summarize the intended behavior with examples, important edge cases, alternatives and drawbacks, compatibility, and relevant validation or performance expectations. Discuss ownership, cancellation and CPU/GPU support where they apply; a host-only API does not need a device implementation. Unresolved questions should remain visible rather than being treated as settled.

The project lead or an explicitly delegated maintainer records the decision in an issue comment, explaining what was accepted or declined and why, with links to relevant discussion. Reactions are feedback, not a vote. Keep the issue body current with the proposal and link to the decision comment. Acceptance of a design does not mean it has been implemented or released: link implementation PRs and record the first release separately. Close the issue with a reason when its work is complete or the proposal is declined or withdrawn.

Prototype branches and experimental PRs can help evaluate an idea before a decision. Implementation PRs should link to the design issue. If implementation changes the agreed behavior, discuss the amendment and record a new decision before merging it. Existing design documents remain useful references; they do not need retrospective RFCs.

## Develop and test

Use the pinned LLVM/MLIR/Clang toolchain with CMake and Ninja:

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

Keep lexer/parser, semantic analysis, typed IR, lowering and runtime responsibilities separate. Preserve source locations, value and mutation rules, checked failures and ownership through worker and device boundaries. Do not expose internal allocation or ABI details as language features without an agreed design.

Add tests for meaningful behavior changes and regressions. Use relevant debug, optimized and sanitizer configurations, and document the scope of qualification honestly. GPU tests need access to the target hardware. The numerical contract applies to every backend.

Keep native design documentation current with the implementation. Write user-facing documentation in Tiramisu under `apps/website/src/content/native`, using examples checked by the native compiler. Organize published pages as tutorials, language reference, built-in reference, HOWTOs, tool usage or contributor guidance. Keep organization planning, qualification reports and experiment notes in repository documentation, not published lessons. Every built-in needs a signature, behavior, failure/placement rules and an entry in `tests/fixtures/public-api.json`.

Run `bun run check` for website and documentation changes. Run `bun run verify:docs --gpu` on a supported Mac with Metal tools to execute the authored examples; omit `--gpu` for CPU-only verification, which reports the GPU execution gap explicitly. Register expected example outputs in `tests/fixtures/docs-examples.json`. The website uses Bun independently of the native CMake build.

Submit focused changes with a clear explanation of behavior, validation and remaining limitations. Please retain project history and contributor attribution.
