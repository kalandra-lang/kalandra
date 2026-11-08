# Explicit returns and primitive names

Primitive numeric types are spelled `i8`, `i16`, `i32`, `i64`, `u8`,
`u16`, `u32`, `u64`, `f32`, and `f64`. Boolean and single-value types
are `bool` and `unit`. Other named types retain PascalCase, including
`String`, `Tensor`, records, enums, and error families. The old numeric
spellings and the `int`, `uint`, and `float` aliases are removed.
Unannotated integer and floating literals continue to infer i64 and f64.

The only value of `unit` is `()`. A unit function can complete naturally
or execute bare `return`, equivalent to `return ()`.

Every function producing a non-unit result must return explicitly on
every completing path. Functions and anonymous functions share this rule.
The final expression of a function body is no longer an implicit return.
Paths which fail or never complete do not require a return.

```kalandra
fn clampLow(x: i64) -> i64:
  return if x > 10:
    x
  else:
    0
```

Blocks used as expressions retain their existing result rules. The
operand of return may be an if, match, parallel group, or loop expression.
Return exits the innermost lexical function, including when it appears
inside a loop or task group. Anonymous functions define new return
boundaries.

Returning preserves structured task settlement, read-access lifetimes,
owned-resource cleanup, checked faults, cancellation, and error precedence.
CPU, Metal, Vulkan, and CUDA functions follow the same language rules.

The migration covers source fixtures, generated Kalandra source, diagnostics,
formatting, editor tooling, documentation, and qualification harnesses.
Backend ABI identities, numerical precision, and C++/shader type names
retain their existing meanings. Entry-point output and process status are
the subject of a separate proposal.
