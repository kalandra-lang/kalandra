# Native String contract

CPU String values, equality, concatenation, length queries, scalar indexing and slicing implemented — 2026-10-18. Literals, annotations, synchronous helpers, `+`, `==`, `!=`, `length`, `byteLength`, scalar indexing, slicing and CLI results are available. Local CPU workers, callback captures and String record fields are available; [String arrays and maps](string-arrays.md) are also available; [String-array record fields](record-string-array-fields.md) are also supported.

The design follows the immutable [core](core.md) and [automatic memory model](memory.md). One ordinary `String` type represents text. No separate `Char` type, allocator arguments, mutable string buffers or lifetime annotations are introduced.

## Text and literals

A String is a finite sequence of Unicode scalar values, stored as well-formed UTF-8. A scalar excludes surrogate code points; UTF-8 uses one to four bytes per scalar. Validation follows [RFC 3629](https://www.rfc-editor.org/rfc/rfc3629): reject overlong encodings, isolated continuation bytes, truncated sequences, surrogate encodings and values above U+10FFFF. Noncharacters and unassigned scalar values remain valid; UTF-8 validity must not depend on a changing character database.

Double quotes delimit a literal. Direct Unicode text and the following escapes are allowed:

| Escape           | Value                                   |
| ---------------- | --------------------------------------- |
| `\"`             | Double quote                            |
| `\\`             | Backslash                               |
| `\n`, `\r`, `\t` | LF, CR, tab                             |
| `\0`             | U+0000                                  |
| `\u{H…}`         | One scalar, with 1–6 hexadecimal digits |

Hex digits are case-insensitive. Unknown escapes, empty/oversized Unicode escapes, surrogate escapes and out-of-range escapes are compile errors. Raw LF/CR inside a literal is rejected; multiline/raw strings and interpolation are separate work. There is no `\xNN` byte escape or UTF-16 surrogate-pair escape syntax. Source byte spans remain available for diagnostics after decoding.

Embedded U+0000 is ordinary text. Strings are length-delimited, not C strings. No BOM is inserted or removed from a String; U+FEFF within a literal is data. There is no automatic newline conversion, normalization or replacement of invalid input. A future lossy decoder must have an explicit name; external byte decoding requires its own recoverable error contract before it is exposed.

## Operations

All operations in this table compile on the CPU backend, including local worker bodies and String callback captures. [String record fields](record-string-fields.md) also work; [String arrays and maps](string-arrays.md) also work.

```kalandra
fn greeting(name: String) -> String:
  "Hello, " + name

fn main() -> String:
  greeting("Kalandra")
```

`String` annotations, inferred literals, ordinary helper arguments/results and immutable bindings follow existing core rules. Passing a number or Bool where String is required is a type error; there is no implicit formatting or parsing.

| Expression                       | Result / rule                                            |
| -------------------------------- | -------------------------------------------------------- |
| `length(text)`                   | `int64` number of Unicode scalars                        |
| `byteLength(text)`               | `int64` number of UTF-8 bytes                            |
| `text[index]`                    | A String containing one scalar                           |
| `slice(text, start, end)`        | String over the half-open scalar interval `[start, end)` |
| `left + right`                   | Concatenate two Strings, preserving both operands        |
| `left == right`, `left != right` | Exact scalar-sequence equality/inequality                |

Index and slice operands accept completed int32/uint32/int64/uint64 scalars. Negative signed indexes, values above INT64_MAX, and out-of-range positions cause a checked Bounds fault; known-invalid constant cases are compile errors. Indexing requires `index < length(text)`. Slicing requires `0 <= start <= end <= length(text)`; it neither clamps nor wraps. An empty slice at the end is valid. `slice(text, 0, length(text))` equals `text`.

String length is not array/tensor shape metadata. A String is not `[uint8]` or `[String]`, has no tensor residence, and cannot be indexed as bytes. Numeric conversions, arithmetic other than String concatenation, whole-string logical operators and implicit array/tensor construction are rejected. Ordering comparisons are deferred; equality is not locale-sensitive collation.

## Unicode behavior without hidden transformations

Use “scalar” precisely; do not describe `length` as a count of visible characters. [Unicode text segmentation](https://www.unicode.org/reports/tr29/) defines grapheme clusters separately. A combining mark or a joiner may be a distinct scalar within one visible unit. Source order is preserved even for right-to-left text.

| Text         | Scalars | UTF-8 bytes | Selected result                          |
| ------------ | ------: | ----------: | ---------------------------------------- |
| `""`         |       0 |           0 | `slice("", 0, 0) == ""`                  |
| `"hello"`    |       5 |           5 | `"hello"[1] == "e"`                      |
| `"سلام"`     |       4 |           8 | `"سلام"[0] == "س"`                       |
| `"é"`        |       1 |           2 | Index 0 returns `"é"`                    |
| `"e\u{301}"` |       2 |           3 | Index 1 returns the combining mark alone |
| `"👩‍💻"`       |       3 |          11 | Index 1 returns U+200D, the joiner       |
| `"a\0b"`     |       3 |           3 | Index 1 returns `"\0"`                   |

`"é" != "e\u{301}"`. Normalization can make distinct sequences comparable in other ways, but String equality does not perform it. [Unicode normalization](https://www.unicode.org/reports/tr15/) and grapheme-aware APIs belong in explicitly named library operations, with pinned Unicode data and independent conformance tests when introduced. The language core does not silently change text, equality or scalar indexing when such tables are updated.

## Cost, ownership and failure

The initial flat UTF-8 implementation may scan for scalar length, indexing and slice boundaries. Budget `length` and scalar-position lookup as O(n), not O(1). The byte count is stored and can be read in O(1), but the initial checked runtime revalidates UTF-8 and storage for both queries, making `byteLength` O(n) at this boundary too. Eliminating repeated validation for trusted immutable values is later optimization work. Equality is at most linear in bytes and concatenation at most linear in the combined byte length. Repeated `text[i]` in a loop can therefore be quadratic. A future scalar iterator should scan once; do not expose an unimplemented iterator API now. Cached counts, indexing accelerators and shared slices are optimizations, not extra source syntax.

Both byte and scalar counts must fit nonnegative int64. Runtime resource budgets may be smaller. Check combined byte counts before concatenation or allocation. Allocation/resource failure uses the existing checked-fault and settlement rules; partial text must never escape. No operation can manufacture an ill-formed UTF-8 String or split a scalar encoding.

A returned String remains valid after its helper returns. Captures, slices, records, arrays and unfinished tasks must keep required backing alive as those integrations are implemented. A slice may share storage, provided its lifetime is retained and its boundaries are valid. Copying is also permitted; pointer identity is not observable. Static literal storage and invocation-owned storage need explicit trusted provenance internally. Never accept arbitrary native pointers as static strings merely because an ownership field is null.

Worker handoff must carry validated lengths/provenance, retain the backing and leave the destination unpublished on failure. Dynamic allocation, scans and copies use the existing cooperative cancellation machinery. Local CPU task diagnostic messages/reasons now use the same managed storage as ordinary Strings. Placement-aware diagnostic internals remain private, and ordinary String use with GPU placement stays gated. Neither representation is a public ABI.

A String-valued CLI `main` writes its UTF-8 bytes followed by one LF, without quotes or NUL truncation. Validate and settle the result ownership before output, as with collection results. [String array display](string-arrays.md#executable-display) uses quoted, escaped elements. Whole-record display remains separate. Native String support initially targets CPU execution; GPU strings, distributed contracts and public C/C++ interfaces are outside this step.

## Implementation sequence and gates

1. **Literal/value foundation — implemented:** String AST and strict escape decoding, typed String literals/annotations, trusted literal and managed storage validation, synchronous helpers/returns and CLI output. Adapt existing internal diagnostic String values or explicitly gate their ordinary use until safe. Prove empty/non-ASCII/NUL behavior and failures before exposing further composition.
2. **Value operations — implemented:** exact equality and concatenation are implemented with checked allocation/counts and fault ordering. Byte/scalar lengths are also implemented. Scalar indexing and half-open slicing are also implemented. Compound integrations below keep their own qualification gates.
3. **Workers and captures — implemented:** owned local CPU packets/results, source settlement, cached outcomes, cancellation, allocation failure and no partial publication; ordinary callback captures and managed CPU diagnostic messages/reasons.
4. **Compound values — partial:** [String record fields](record-string-fields.md), including nested records, record arrays/maps and retained CPU worker transfer, are implemented. [Direct String arrays and String-element maps](string-arrays.md) are also implemented. [String-array fields in records](record-string-array-fields.md) also retain their arrays and text recursively.
5. **Text libraries and optimization:** explicit decoding/encoding once byte types and recoverable errors are ready; normalization, grapheme iteration and locale behavior with pinned tables; shared slices, cached counts and linear traversal. Add broader library operations independently.

[Conformance vectors](fixtures/strings-semantics.json) record scalar/byte counts, exact equality, index/slice results, invalid UTF-8, literal escape cases and bounds decisions for future tests. The literal/value, equality/concatenation and length tests consume their corresponding text, escape, count and invalid UTF-8 vectors; scalar index and slice vectors are consumed by their corresponding qualified tests. Qualification must also cover all scalar boundary widths, cancellation during long scans, failure cleanup, external descriptor validation and result lifetime. Reuse generic ownership/scheduling machinery rather than building a separate String runtime for each context.

## Qualified literal/value foundation

```kalandra
fn identity(value: String) -> String:
  value

fn main() -> String:
  identity("سلام\0👩\u{200d}💻")
```

This compiles on the CPU backend. The executable prints the literal UTF-8 bytes, including the embedded NUL, followed by LF. Ordinary bindings, inferred results, conditional selection and explicit relative file-module imports also work. `String` is a reserved builtin type name. The existing literal resource limit applies to the quoted source token (4,096 bytes by default).

The first storage path copies compiler-emitted literal globals into invocation-owned immutable buffers. A private length-delimited descriptor carries the allocation base, data pointer and byte length; nonempty values require a known owner, matching base/data and an in-bounds length before UTF-8 validation touches the bytes. Empty values use the canonical null/null/zero descriptor. No arbitrary pointer is treated as static storage. Helpers share these immutable buffers within the invocation; the CLI retains the returned backing, settles invocation owners, then writes the result. Earlier temporary Strings settle at the same invocation boundary. Earlier reclamation and static-literal reuse are later optimizations.

Source Strings support synchronous helpers and local CPU task modules. Placement-aware functions and ordering comparisons remain unsupported. Callback captures and maps support numeric/Bool/record/String elements; see [String arrays](string-arrays.md). Exact equality, concatenation, length queries, scalar indexing and slicing are qualified below.

Qualification covers debug, optimized generated code, address/undefined sanitizers and thread sanitizer: exact Unicode/NUL/BOM bytes and scalar encoding boundaries; strict escape/UTF-8 rejection; imported helpers and conditional results; raw CLI output and scalar/collection cleanup; invocation isolation, retained result lifetime and malformed native descriptors; allocation failure, cancellation during scans/copies and unpublished failed results. Rejected literal builds preserve prior executable/MLIR/LLVM artifacts. See the [recorded evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-values-2026-10-18.json).

## Exact equality and concatenation

```kalandra
fn greeting(name: String) -> String:
  "Hello, " + name

fn main() -> Bool:
  greeting("Kalandra") == "Hello, Kalandra"
```

`+` joins two completed Strings; `==` and `!=` compare exact scalar sequences and return Bool. Valid UTF-8 has one encoding for each scalar sequence, so byte equality implements this rule without normalization, locale rules or NUL termination. Numeric/Bool mixing, ordering comparisons and other arithmetic operators are rejected. Existing operator precedence applies: `a + b == c` compares the joined String with `c`.

Operands evaluate once, from left to right. The checked call boundary stops evaluation after a fault or cancellation. Equality validates both complete operands before using unequal-length or unequal-prefix shortcuts; malformed trailing bytes cannot hide behind an earlier mismatch. Equality allocates no storage. Concatenation validates both inputs, checks that the combined byte length fits nonnegative int64, then copies into new managed storage. Two empty inputs produce the canonical empty String; a single empty input still follows the initial copying path. Inputs remain unchanged, and a result is published only after the copy completes. Count/allocation failure reports AllocationFault; invalid private descriptors report RuntimeFault.

Validation, comparison and copying poll cooperatively. Qualification interrupts large comparisons/copies after input validation and checks that interrupted concatenation leaves its output empty and settles partial owners. Tests cover exact Unicode/NUL behavior, all equality/concatenation conformance vectors, imported/chained helpers, short-circuit logic, synchronous map callbacks, source fault order, malformed descriptors, count overflow and allocation failure. Local CPU task diagnostic equality now uses managed String storage, as described in the worker section below. See the [operation evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-operations-2026-10-18.json).

## Scalar and byte lengths

```kalandra
fn main():
  text = "👩‍💻"
  [length(text), byteLength(text)]
```

This prints `[3, 11]`. `length` counts Unicode scalars; `byteLength` counts UTF-8 bytes. Both return scalar `int64`, including zero for the empty String. Neither counts visible grapheme clusters. A combining mark, a joiner, NUL and a BOM each count as one scalar; no text is stripped or normalized. `length` retains its existing array behavior, and `byteLength` accepts only String values. String `shape` remains unsupported; scalar indexing and slicing are qualified separately below.

Each query evaluates its argument once. It validates the complete owner/descriptor and UTF-8 sequence before returning a count; malformed trailing bytes cannot produce a plausible byte length. Scalar counting shares that validation scan, publishes its count only after successful validation, and polls cooperatively. There is no count cache or additional managed allocation. The String byte-length bound also bounds the scalar count to nonnegative int64. Failed or cancelled scans return through the existing checked invocation boundary and settle their owners normally.

The queries compose with synchronous helpers, relative module imports, pipelines, conditionals and map callbacks. The worker integration below extends these queries to local CPU tasks and managed task diagnostics. Qualification covers the complete text/count vectors, Unicode encoding boundaries, embedded NULs, no additional allocation, single argument evaluation, source fault ordering, malformed descriptors/UTF-8, cancellation during long scans, allocation failure in argument construction, and preservation of existing outputs on rejected builds. See the [length evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-lengths-2026-10-18.json).

## Scalar indexing

```kalandra
fn main() -> String:
  "aé👩"[2]
```

This prints `👩`. A String accepts exactly one completed int32/uint32/int64/uint64 index. Indexes count Unicode scalars from zero, and the result is a String containing one scalar. No separate Char type is introduced. A combining mark or joiner is returned by itself when indexed: `"👩‍💻"[1]` is U+200D. Embedded NUL and BOM scalars remain ordinary values.

Negative signed indexes, high uint64 values and positions at or beyond the scalar count fail with BoundsFault. Known negative/high constants, empty literals and out-of-range literal/binding/concatenation indexes are rejected as compile errors. The frontend tracks scalar extents for literals, immutable bindings, known concatenations and successful indexing results; it does not propagate helper argument constants between functions. Dynamic helper inputs retain runtime bounds checks. Source and index expressions evaluate once, from left to right; an earlier fault prevents later evaluation.

The checked runtime locates the scalar during complete UTF-8 validation, including bytes after the selected position. Unknown/settled owners, malformed descriptors and invalid trailing UTF-8 report RuntimeFault before any result is allocated. A valid position then copies exactly one to four bytes into new invocation-owned immutable storage. The result survives its helper and parent-buffer lifetime, and can be retained past invocation settlement. Allocation failure reports AllocationFault. Cancelled/failed scans never publish a selection or result. Validation polls cooperatively; scalar indexing is O(n) in the initial implementation even for position zero.

This is a copying implementation. Earlier temporary reclamation and safe shared-storage views remain optimization work; retaining the indexed result does not require retaining its entire parent. Source buffers still follow the current invocation cleanup policy. Local CPU workers/captures are qualified below; String record fields are now supported; [direct String arrays](string-arrays.md) are also supported. Slicing is qualified below. Qualification covers all scalar-index conformance vectors, four integer widths, dynamic bounds, escaped/Unicode/NUL/BOM text, source evaluation and fault order, imported helpers, sequential callbacks, parent release, retained results, allocation failure, complete malformed-input rejection, cancellation, and rejected-build output preservation. See the [indexing evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-indexing-2026-10-18.json).

## Half-open scalar slicing

```kalandra
fn main() -> String:
  slice("aé👩", 1, 3)
```

This prints `é👩`. `slice(text, start, end)` selects Unicode scalars in `[start, end)`. Each bound independently accepts int32, uint32, int64 or uint64. A valid range satisfies `0 <= start <= end <= length(text)`. Empty ranges, including `slice(text, length(text), length(text))` and `slice("", 0, 0)`, return the canonical empty String without result allocation. Bounds are neither clamped nor wrapped.

Known negative/high bounds, reversed ranges and bounds beyond a tracked scalar extent are compile errors. Other invalid ranges cause BoundsFault. Known endpoint differences also supply scalar extents for subsequent bindings/indexing, while dynamic ranges keep runtime checks. Wrong source/bound types and arity are rejected. Source, start and end expressions evaluate once in that order; a fault stops later evaluation.

Both scalar boundaries are located during complete UTF-8 validation. Even an empty range validates the full input, so malformed trailing text cannot be discarded silently. Invalid descriptors or UTF-8 report RuntimeFault before allocation. The initial implementation copies nonempty ranges into independent invocation-owned buffers; full-range slicing also copies. Parent/result lifetimes follow the existing managed model, and a retained result does not require retaining its parent. Allocation failure reports AllocationFault, and interrupted scans/copies leave the result unpublished and settle partial owners. Validation and copying poll cooperatively.

Slicing composes with imported helpers, pipelines, sequential callbacks, equality, lengths and indexing. The worker integration below also supports slicing inside local CPU tasks and callbacks with String captures. String record fields are now supported; [direct String arrays](string-arrays.md) are also supported. Qualification includes all 18 design slice vectors, 16 mixed integer-bound signatures, 55 scalar intervals spanning ASCII, Unicode, joiners, NUL and BOM, empty/end/full ranges, source evaluation/fault order, retained ownership, malformed-input rejection, allocation failure, cancellation during scan/copy, and rejected-build output preservation. See the [slicing evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-slicing-2026-10-18.json).

## Local CPU workers and captures

```kalandra
fn greet(name: String) -> String:
  "Hello, " + name

fn main() -> String ! TaskError:
  try parallel:
    work = spawn greet("Kalandra")
    first = try await work
    again = try await work
    first + " / " + again
```

String arguments and completed results share immutable backing through checked references in the existing CPU task scheduler. Submission retains input storage before returning; callers can settle their own invocation without invalidating the task. Results remain valid across nested helpers, cached/repeated awaits, lexical group release and scheduler destruction while the receiving invocation still owns them. Empty Strings remain canonical and require no backing allocation. This shares bytes instead of copying the entire String at each handoff; descriptor/owner bookkeeping still has a cost.

Each successful packet/result copy validates source ownership, length and the entire UTF-8 input, then acquires the destination reference before publishing its descriptor. A fault, cancelled scan, failed owner allocation or wrong result layout leaves the destination output untouched. Partially acquired dependencies settle through the existing invocation/task cleanup. Revalidating immutable bytes on each handoff is conservative; reducing that cost belongs to the later trusted-provenance optimization.

Completed Strings can also be captured by sequential anonymous map callbacks, including a callback running inside a worker. The input and result arrays still need supported numeric, Bool or record elements:

```kalandra
fn sizes(label: String) -> [int64]:
  map([1, 2, 3], fn(value): length(label) + value)
```

CPU task diagnostic `message` and `reason` bindings are copied from trusted runtime message constants into ordinary managed Strings. They can be queried, sliced, returned, captured and passed to a later worker without leaving a borrowed pointer into an expired task. Diagnostic materialization can itself fail with AllocationFault and follows the same cancellation rules. String-valued success/recovery/rethrow paths use the same structured TaskError rules as numeric values.

Qualification covers empty, ASCII, Unicode, embedded NUL and large inputs; 1/2/4-worker pools; nested and sibling tasks; repeated observations; source/group/scheduler destruction; malformed descriptors; wrong layouts; cancelled validation; cancellation/deadlines; task admission and owner allocation failures; callback captures; imported helpers; and recovered diagnostic Strings. See the [worker evidence](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-string-workers-2026-10-18.json).

[String record fields](record-string-fields.md) now use this managed worker foundation. [Direct String arrays](string-arrays.md) also use this foundation. [String-array record fields](record-string-array-fields.md) are also supported. GPU Strings, native distributed execution and public native interfaces remain separate work. No distributed transport is introduced here.
