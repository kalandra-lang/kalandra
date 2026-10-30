# Immutable native String arrays

`[String]` is a rank-one immutable CPU array of Strings. Literals, typed helpers, indexing, `length`, conditionals, named/anonymous maps, lexical captures and local worker arguments/results are supported.

```kalandra
fn decorate(text: String) -> String:
  text + "!"

fn main() -> [String] ! TaskError:
  names = ["north", "south"]
  try parallel:
    work = spawn map(names, decorate)
    first = try await work
    try await work
```

This prints `["north!", "south!"]`. `map(names, decorate)` and `names -> map(decorate)` are equivalent. Maps can consume or produce Strings alongside the supported numeric, Bool and record element types. Mapping Strings to records containing String fields, then mapping those records back to Strings, also works. Map callbacks remain synchronous and have one source element parameter; existing anonymous callbacks can capture completed String arrays, including from inside workers.

## Values and operations

- `["é", "👩", ""]` infers `[String]`. Mixed values such as `["a", 1]` are rejected; there is no implicit formatting.
- Empty literals need a type context: `names: [String] = []`, an explicitly typed return, a named String callback, or an annotated anonymous callback parameter.
- `length(names)` returns the number of Strings as `int64`. `length(names[0])` counts scalars in the selected String.
- `names[index]` returns a String. It accepts the existing four integer index types and uses checked array bounds. A subsequent `names[index][position]` uses String scalar bounds.
- Empty maps do not invoke the callback. Output arrays have the same number of elements as their input; returning an array from a callback does not flatten it.

Arrays cannot be concatenated with String `+`, compared as whole values, converted to tensors or indexed as text. `byteLength` and `slice` still require a scalar String. Nested arrays, String tensors and GPU String arrays remain separate work. [String-array fields in records](record-string-array-fields.md) are supported. Arrays of records containing scalar String fields remain supported.

## Ownership and failure

The private array backing stores String descriptors, rather than concatenating text into one buffer. Text bytes remain immutable and can be shared by multiple elements. Worker packet/result adapters retain the array backing and each nonempty String dependency before publishing an array. Empty arrays have canonical empty descriptors. Empty String elements need no text backing.

Validation checks the complete outer descriptor and every element's ownership, byte length and UTF-8, with cooperative cancellation. A malformed later element, interrupted scan or failed retention must leave the destination unpublished; acquired references settle with the receiving invocation. Cached/repeated awaits, source settlement, group release and scheduler destruction preserve results while the receiving invocation owns them. A selected String can be returned independently of its array's backing.

Map construction uses the same invocation ownership ledger. A failing callback does not publish a partially initialized array, and temporary text/array backing settles on the failure path. This supports structured TaskError recovery/rethrow and arrays containing managed diagnostic messages.

The initial implementation revalidates arrays at helper and handoff boundaries and retains each element separately, even when several elements share text. Avoiding redundant scans and owner entries is later optimization work; pointer sharing does not imply constant-time handoff.

## Executable display

A `[String]` main result prints quoted elements and preserves UTF-8. Quotes, backslashes, LF, CR and tabs are escaped; other ASCII controls, including NUL and DEL, use `\u00xx` escapes. A scalar String main keeps its existing raw UTF-8 output behavior.

Collection display options apply: the default shows up to 100 elements, `--limit N` changes that bound, and `--all` shows every element. Omitted elements are represented by `...`. The complete array is validated and retained, and source owners are settled, before any output is printed—even with `--limit 0`. Write errors can still interrupt output after printing begins.

## Qualification

The focused suite covers empty/Unicode/NUL values, nested worker and cached results, shared backing, mixed maps, captures, imported helpers, strict typing and forged IR, partial retention failure, malformed late elements, cancellation during long scans, allocation failure, recovery and escaped CLI output. A private CLI fixture rejects a hidden malformed tail at multiple display limits before printing. Rejected builds preserve existing output artifacts. The example above is compiled and executed in every build variant.

See the [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-arrays-2026-10-18.json) for the qualified checks and artifacts. [String-array fields in records](record-string-array-fields.md) are also supported; distributed execution and public C/C++ interfaces remain separate roadmaps.
