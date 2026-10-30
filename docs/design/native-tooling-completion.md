# Native unfinished-code tooling completion

The N4 unfinished-code gate covers the current native language on macOS ARM64:
checked completion and signature help while an expression is being edited,
isolation from unrelated broken function bodies, current source overlays, real
diagnostics, and debug/sanitizer/protocol/installed-editor acceptance. Distribution
and other platforms remain in N5; website, onboarding and release work remain in
N6. This gate does not promise suggestions for every arbitrary sequence of tokens.

## Recovery contract

Completion and signature requests may load a request-local graph with
`SyntaxLimits::editorRecovery`. The lexer retains delimiter diagnostics and may
insert zero-width closers for at most eight open delimiters. Mismatched closers
may close intervening delimiters. A top-level named declaration fences an
unfinished preceding declaration, including across composed module sources.
Anonymous-function layouts unwind before synthetic closers.

The parser retains its diagnostics and recognizes missing expression/member
slots, delimiters, colons, suite boundaries and conditional branches. Expression
holes are unresolved zero-width names, never guessed literals or typed values.
Statement synchronization and progress guards bound recovery loops. Ordinary
source-byte, token, expression/type-node, nesting and diagnostic budgets still
apply; hitting a resource limit disables recovery queries. Non-delimiter lexical
errors, including unterminated strings and invalid characters, fail closed.

The checker registers declarations and checks the function containing the query
first. It resolves required callees and receivers normally. A checked query may
stop before independent broken function bodies; errors in the query's required
prefix/dependencies prevent a snapshot. Global declaration/signature errors can
also prevent useful guidance. Completion names describe available bindings and
members, rather than proving that a whole unfinished expression type-checks.

Compound signature slots use the same checked expression traversal with the
callee's real parameter context. This covers arithmetic, comparison, logical,
index/collection, closure and control-expression paths where the checker can
reach the selected name, field or empty operand. It retains preceding-argument
validation and ordinary evaluation/type-inference order. Empty member slots
still require a checked nominal receiver. Type-dependent builtin signatures
continue to require sufficient genuine input information.

`Module::editorOnly` cannot produce executable typed IR. The ordinary checker
rejects it even when the recovered syntax has no diagnostics. Query checking
always discards its typed module and exposes only a scope or callable-signature
snapshot. Existing strict signature paths remain available for complete calls
and supported builtin operations.

Actual document diagnostics, hover, navigation, rename and formatting continue
through strict checking. Recovery never publishes probe diagnostics, writes
buffers/files, supplies fake runtime arguments or reuses a previous version's
module. Completion ranges replace the selected original identifier and preserve
operators, suffix boundaries and surrounding source. Current imported overlays
and configured package graph policies apply to query-local graphs as well.

Expression scope completion also offers `true`, `false`, `if`, `match`, `fn` and
`not`, with LSP Keyword item kinds. Lexical bindings retain priority. Member,
type, label and call-target completion retain their contextual candidates;
compiler call forms are not advertised as first-class function values.

## Exit checklist

- [x] Request-local structural recovery and independent declaration boundaries.
- [x] Collections/indexing, nested suites/closures, control and compound call slots.
- [x] Checked prefixes, receivers, callable signatures and live imported overlays.
- [x] Contextual expression keywords and original UTF-16 replacement ranges.
- [x] Strict diagnostics and no executable partial IR or stale semantic queries.
- [x] Resource-limit rejection and a deterministic 512-case malformed-input corpus.
- [x] Debug and ASan/UBSan unit/protocol regression suites.
- [x] Relocated Release tools and packed VSIX in installed VS Code on macOS ARM64.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-lsp-recovery-2026-10-23.json)
records source hashes, selected test results and installed-editor checks. Existing
completion/signature suites retain coverage for aliases, nominal/error types,
higher-order callables, pipelines, package boundaries and placements. This is
focused tooling qualification; it does not claim a new GPU or full native
compiler conformance run.
