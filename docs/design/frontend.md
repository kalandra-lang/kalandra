# Handwritten native syntax frontend

Implemented first syntax subset — 2026-10-15. The [CPU milestone](cpu-milestone.md) now has a handwritten C++23 lexer and recursive-descent/Pratt parser. No parser generator, JavaScript parser, Bun, or Node is involved. This frontend constructs an AST consumed by the [implemented semantic stage](semantics.md). The [native backend](native-codegen.md) now lowers the checked CPU subset into executable code.

## Run

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
build/native/native/kalandra-parse docs/design/fixtures/cpu-projection.k
```

The projection fixture reports `Parsed 4 functions, 40 expressions (syntax only)`. Parse failures print source path, one-based line/byte column, diagnostic code, source excerpt, and caret to stderr, and return a nonzero exit status. The tool is an internal frontend entry point, not the final compiler CLI.

## Grammar of the implemented subset

The simplified grammar below describes syntax, not semantic validity. NEWLINE, INDENT, DEDENT, and EOF are lexer tokens. Identifiers in type positions remain unresolved names.

```ebnf
module      = NEWLINE*, (function | import), (NEWLINE* (function | import))*, NEWLINE*, EOF ;
import      = "from", string, "import", imported, (",", imported)*, NEWLINE ;
imported    = identifier, ("as", identifier)? ;
function    = "export"?, "fn", identifier, "(", parameters?, ")",
              ("->", type)?, ":", NEWLINE, INDENT, statement+, DEDENT ;
parameters  = parameter, (",", parameter)*, ","? ;
parameter   = identifier, ":", type ;
type        = identifier
            | "Tensor", "[", identifier, (",", dimension)*, "]" ;
dimension   = integer | "?" ;
statement   = (identifier, (":", type)?, "=")?, expression, NEWLINE ;
expression  = pipeline ;
pipeline    = sum, ("->", sum)* ;
sum         = product, (("+" | "-"), product)* ;
product     = unary, (("*" | "/" | "%"), unary)* ;
unary       = ("+" | "-"), unary | call ;
call        = primary, ("(", arguments?, ")")* ;
primary     = identifier | number | "(", expression, ")"
            | "[", arguments?, "]" ;
arguments   = expression, (",", expression)*, ","? ;
```

A function body must end in an expression statement. The grammar's generic type-name alternative does not allow bare Tensor without its brackets. A tensor element name is retained separately from its dimensions. Dimensions must be decimal, nonnegative uint64-representable integers or `?`; this is syntactic validation, not a claim every extent can be allocated.

The [function-type parser](function-values.md#implementation-status) extends type syntax with recursive callable signatures and grouping:

```ebnf
type         = identifier | array-type | tensor-type | "(", type, ")"
             | "fn", "(", type-list?, ")", "->", type, error-channel? ;
type-list    = type, (",", type)*, ","? ;
error-channel = "!", (identifier | "(", identifier, ("|", identifier)*, ")") ;
anonymous    = "fn", "(", anonymous-parameters?, ")", ("->", type)?,
               error-channel?, ":", suite ;
anonymous-parameters = anonymous-parameter, (",", anonymous-parameter)*, ","? ;
anonymous-parameter = identifier, (":", type)? ;
suite        = expression | NEWLINE, INDENT, statement+, DEDENT ;
```

Function arrows associate to the right. An error suffix belongs to the innermost ungrouped callable; group a returned function type before adding its factory's error channel. Repeated ungrouped suffixes are rejected. The [named CPU function-value subset](native-function-values.md) now implements the supported scalar, record and collection signatures and typed indirect calls. Parsing broader signatures does not imply execution support; expected-error callable signatures resolve canonical error sets, and [managed expression closures](native-closures.md) add lexical captures and expected-signature inference. [Anonymous bodies](native-block-closures.md) use the same statement-suite AST as other bodies. [Direct anonymous invocation error annotations](anonymous-errors.md) now follow the same suffix/grouping rules. [Function-valued record and enum fields](native-callable-fields.md) now resolve and execute with exact signatures. [Array element types](native-function-arrays.md) can now contain recursive callable signatures. Pipelines accept function-valued fields, indexed functions and calls using the same first-argument insertion rule.

Calls and postfix indexing bind most tightly, then unary signs, multiplication/division/remainder, addition/subtraction, and pipelines. Binary arithmetic and pipeline operators associate left to right. Pipelines remain distinct AST nodes; first-argument insertion belongs to later typed lowering, not parser-side execution or rewriting that loses source information.

Bindings are statements only. `foo(x = 1)` is rejected, as is the prototype's `=>` form. General arrays are syntactically represented to support tensor-constructor literals; the parser does not claim runtime array support or reject ragged literals as though it had checked shapes.

## Lexical behavior

- Source must be valid UTF-8. Initial identifiers use ASCII letters/underscore followed by letters/digits/underscore; Unicode identifiers need a later explicit policy. Unicode text in comments and lexed strings is accepted.
- LF and CRLF are supported. Indentation uses spaces, with each dedent required to match an enclosing width. The first width is not fixed at two or four spaces; tabs outside comments/strings are diagnosed.
- Blank lines and comment-only lines do not alter indentation. `#` starts a line comment outside a string.
- Newline and indentation tokens are suppressed inside matching parentheses, brackets, and braces. Delimiter mismatch/unclosed-delimiter diagnostics retain source spans.
- Numbers retain their original spelling: decimal whole numbers and decimal/exponent forms are tokenized. Numeric range, default family, float32 rounding, and context typing remain semantic work. Hex literals and suffixes are not in this first subset.
- Strings are tokenized to retain lexical boundaries and diagnose unterminated quotes, but string expressions are explicitly unsupported by this parser. Escape decoding and general string semantics are not implemented here.

[Numeric array and tensor indexing](tensor-indexing.md) now parse as postfix input/index-list expressions; semantic checking enforces the rank and index types. Inline anonymous functions in numeric maps and built-in TaskError handlers are also implemented. General field access, record construction, user-defined expected-error declarations and other non-function top-level declarations remain unsupported. Their rejection is an implementation limit, not a reversal of the accepted designs. Parameter/local duplicate names, unknown functions, dtype compatibility, and matrix-shape requirements belong to the next semantic stage.

## Ownership and diagnostics

A module retains shared immutable ownership of its source file. Tokens and nodes store UTF-8 byte spans instead of borrowing temporary strings. Line offsets are indexed for diagnostic lookup; current columns count bytes. Future editor UTF-16/display-column mapping must be added explicitly rather than reinterpreting these offsets.

Expressions occupy a flat arena and refer to child nodes by indices. Long left-associative chains do not recursively own or destroy children. Numeric spellings, type names, tensor element names, dimensions, bindings, and function signatures remain available for resolution and typed IR.

Lexer errors prevent parsing that token stream, avoiding punctuation/indentation cascades. Parser recovery resumes at statement boundaries or the next top-level function, so independent syntax errors can be reported together. Partial ASTs are for diagnostics only when errors exist; semantic analysis must require a successful parse.

Default configurable limits are 16 MiB of source, 500,000 tokens, 200,000 expression nodes, 128 levels of expression/delimiter/indentation nesting, and 32 diagnostics. Exceeding a limit is diagnosed rather than silently accepting a truncated program. These are frontend resource limits, not language size/rank semantics. The CLI bounds file reads before retaining an oversized source.

## Verification

CTest covers the projection fixture, indentation and continuation tokens, CRLF, comments, numeric/string lexing, invalid UTF-8, exact operator AST structure, tensor type structure, retained source ownership, spans, recovery across functions, unsupported syntax, and every configurable resource category. Semantic-invalid programs are intentionally allowed through syntax when their grammar is valid.

The same unit suite runs with AddressSanitizer and UndefinedBehaviorSanitizer, instrumenting both frontend library and tests. It includes 3,000 deterministic generated malformed inputs and a 20,000-operator chain. CLI tests verify both successful parsing and source/file errors with nonzero exit codes. This is robustness evidence, not a claim of exhaustive fuzzing or a measured speed advantage over other parsers.

## Semantic stage and next step

[Resolution and typed IR](semantics.md) are now implemented for the CPU projection subset, with contextual float32 literals, immutable lexical bindings, resolved calls, tensor extents, and shape checks. The [native backend](native-codegen.md) now generates MLIR from this checked source; broader CPU conformance is next.

## Local task syntax

The parser now also accepts parallel blocks, spawn/await/try prefixes, and `! TaskError` function results. `try await spawn f(args)` groups as `try (await (spawn f(args)))`. A block consumes its indented suite and ending dedent; a following statement is not mistaken for a postfix continuation. Task nodes keep source spans and flat arena references, and parallel blocks retain ordered statement roots. Ownership and error-channel checks belong to [semantic analysis](semantics.md), while [native task lowering](native-tasks.md#compiled-source-task-subset) emits resumable frames. The parser also accepts inline/block `catch fn(error): ...` handlers, `fail error`, and `match` with qualified variants, selected named fields, field-binding aliases, and a final `_` fallback. Semantic analysis currently restricts these to the built-in TaskError/FaultKind families. General enum/error declarations, variant construction, ordinary anonymous functions, other error families, and placement remain outside this implemented subset.

Catch binds less tightly than pipelines (precedence 5 versus 10). A handler's inline body is a complete expression; use parentheses to apply another catch or pipeline to the combined result rather than to the handler body. Match subjects are expressions; arms contain an inline expression or an indented suite. Field patterns accept `{ kind, message }` and `{ kind: cause }`; omitted fields remain unbound. Pattern expressions, guards, and nested destructuring are not implemented.

```kalandra
result catch fn(error):
  match error:
    TaskError.Fault { kind }:
      match kind:
        FaultKind.Allocation: 0
        _: -1
    _: -2
```

The surrounding success type supplies the float32 literal context in this example. Match/catch bodies retain ordered statement roots and lexical bindings in the flat AST. The existing nesting, token, and node limits apply to these constructs too.

## Boolean comparison syntax

The parser now represents `==`, `!=`, `<`, `<=`, `>` and `>=` with a distinct ComparisonExpr and operator identity. Comparisons bind below pipelines and above catch; ungrouped chains receive P034. `true` and `false` are resolved as reserved Bool literals by semantic analysis. See the [CPU Boolean contract](booleans.md) for typing, numerical rules, worker support and current limits.

## Conditional and logical syntax

The CPU subset now accepts `if condition: ... else: ...` with inline or indented branch results, plus Boolean `not`, `and` and `or`. Both branches are required. Comparisons/pipelines bind above logical operators; `not` includes comparisons, `and` binds above `or`, and catch remains below them. ConditionalExpr and LogicalExpr keep explicit branch/operand IDs; semantic lowering retains conditional evaluation. See the [control-flow contract](control-flow.md).

## Local module syntax and loading

[Native local function modules](modules.md) now add top-level named imports with optional aliases and `export fn` declarations. The CPU compiler/checker use a bounded canonical-file graph loader with independent per-file parsing, module-local visibility and original source mappings. The parser alone retains declarations without reading dependencies; semantic checking rejects imports that have not been resolved by the loader.

## Tracked parser follow-up: array expression after a block binding (FRONT-001)

An array literal beginning the next statement after a block-valued binding can currently be mistaken for an unsupported postfix expression and report `P010`. A minimal reproducer is:

```kalandra
fn identity(value: int) -> int:
  value
fn main() -> [int] ! TaskError:
  result = try parallel:
    try await spawn identity(1)
  [result]
```

Binding the literal first (`output = [result]`, followed by `output`) avoids the ambiguity. Fix statement-boundary handling during frontend polish; this is an existing parser defect, not an intended array or lifetime restriction. Record-array execution examples use unambiguous statement boundaries.
