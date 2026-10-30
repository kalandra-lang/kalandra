# Indented anonymous bodies inside delimiters

Implemented native CPU subset — 2026-10-19. Anonymous functions can have indented bodies directly inside calls, grouped expressions, arrays and record fields. Their type inference, captures and execution use the existing [block closure](native-block-closures.md) and [synchronous map](native-control-maps.md) rules.

```kalandra
fn main() -> [int]:
  offset: int = 10

  map([-20, 0, 5], fn(value):
    shifted = value + offset
    if shifted > 0:
      shifted
    else:
      0
  )
```

The [standalone fixture](fixtures/delimited-closures.k) returns `[0, 10, 15]`. There is no need to bind the callback separately, although that form remains supported.

## Layout rules

A newline immediately after the anonymous function's `:` starts its indented body, including when a comment follows the colon. The body must indent beyond the header line and the enclosing active suite. Blank lines and comment-only lines do not start or end a body. The final statement must be a result expression.

Close the surrounding delimiter on a dedented line. If another argument or element follows the callback, its separating comma also belongs outside the body:

```kalandra
fn apply(callback: fn(int) -> int, value: int) -> int:
  callback(value)

fn main() -> int:
  apply(
    fn(value):
      adjusted = value + 1
      adjusted * 2
    , 20
  )
```

This returns `42`. Keeping the comma outside the suite distinguishes call arguments from statements in the callback. A closing delimiter on the final body line, or still indented as part of the body, produces `L016`; dedent it onto its own line instead.

Ordinary multiline argument lists, arrays, records and function-type annotations still ignore line breaks inside their delimiters. Calls and collections nested inside an anonymous body also retain that continuation behavior. Nested anonymous bodies establish their own layout scope when needed. Once their delimiter closes, the enclosing body continues at its prior indentation.

Both direct mapping and higher-order calls can use these bodies. Expected-error declarations, synchronous recovery, matches, returned captures, record/array storage and local-worker execution follow their existing rules. Each mapped element must still be completed before returning. This syntax does not enable task-coordinating callbacks or function transport across workers.

## Implementation and limits

The lexer recognizes anonymous `fn(...)` headers and temporarily restores layout at the delimiter depth containing their bodies. Deeper delimiters continue to suppress layout normally. Dedenting restores the enclosing continuation context without changing its indentation stack. Record fields named `fn` and function types in parameter lists do not create anonymous bodies.

The parser already accepts statement suites for anonymous functions; no parser grammar, backend, runtime or scheduler changes are needed. Existing token, indentation, delimiter, expression/type and diagnostic budgets remain in force. Source spans identify the anonymous body without swallowing its enclosing call's closing delimiter.

This increment supports anonymous-function and `catch fn(error)` suites inside delimiters. Other standalone indented expressions directly inside delimiters, such as an unwrapped `if` argument, are not added here. [Recursive synchronous callables](native-recursive-callables.md) are supported with checked call admission. Function transport, GPU callbacks and public foreign-function interfaces remain separate work.

## Qualification

All 92 selected checks passed on Apple M2 Max. The new execution suites compile 148 programs across debug, optimized, ASan/UBSan and separate ThreadSanitizer builds. Relevant existing parser, semantic, map, module, closure, aggregate, application-error and worker regressions pass in their selected debug and ASan/UBSan modes. This is focused qualification, rather than a full-suite claim.

Each frontend variant checks 24 source programs with LF, CRLF and EOF-without-newline endings, nine syntax rejections, two scope/immutability rejections and 364 truncation prefixes, including partial UTF-8. AST/token spans, balanced layout, lexical captures and compiler budgets are checked. Runtime coverage includes all ten numeric scalar widths, nested calls and callbacks, owned captures, expected errors, direct worker mapping and cached retained results.

The raw invocation harnesses check 56 allocation/context probes, 24 first-fault allocation probes and eight already-stopped contexts. Worker examples run with one, two and four workers in each mode. Thirty-six rejected CLI programs preserve 108 pre-existing output artifacts. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-delimited-closures-2026-10-19.json) records the selected checks, coverage and frozen implementation/test hashes.
