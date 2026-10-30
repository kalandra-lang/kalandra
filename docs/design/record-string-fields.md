# String fields in native records

Immutable CPU records can contain `String` fields, including inside nested records. Construction, field access, immutable updates, local modules, callback captures, record arrays and local worker arguments/results use the existing nominal record rules.

```kalandra
type Label:
  text: String
  enabled: Bool

type Reading:
  label: Label
  value: float64

fn rename(reading: Reading) -> Reading:
  reading with {label: reading.label with {text: reading.label.text + "!"}}

fn main() -> String ! TaskError:
  original = Reading {label: Label {text: "north", enabled: true}, value: 12.5}
  changed = try parallel:
    work = spawn rename(original)
    first = try await work
    again = try await work
    again
  original.label.text + " / " + changed.label.text
```

The executable prints `north / north!`. Updating the nested label preserves the original record and its text. A String field requires a String value; numbers and Bool values are not implicitly formatted. Every declared field is required when constructing a record. The usual String operations apply after field access, with Unicode scalar bounds rather than byte indexes.

## Storage and ownership

A String field stores the private managed String descriptor inline in its record. The text bytes remain in immutable shared backing. Copying a record within an invocation copies its descriptors; crossing an invocation or task boundary retains each String dependency before publishing the complete record. Two fields referencing the same text continue sharing its bytes. Empty fields use canonical empty descriptors and need no text backing allocation.

Nested String fields are validated on entry to and return from ordinary record helpers. CPU task packet/result adapters recursively validate and retain String fields, alongside existing numeric and Bool collection fields. They also traverse records stored in record arrays. Complete UTF-8 validation, ownership checks and cancellation polling apply before a field is retained. An invalid later field or a failed owner allocation must leave the output record unpublished; earlier acquired owners settle with the receiving invocation.

This preserves text across caller settlement, nested worker completion, cached/repeated awaits, group release and scheduler destruction while the receiving invocation still owns the result. String-valued fields can also hold recovered task diagnostic text. No allocator, lifetime annotation or mutable buffer becomes part of the source language.

Record layout limits count a String descriptor as three storage units. Existing depth, expanded-layout and inline-cycle checks remain in force. String fields do not permit recursive record storage cycles.

## Composition and current limits

Arrays of records containing Strings are supported; a map can inspect or update those records and return a supported numeric, Bool or record array. Callbacks can capture an entire record and use its String fields. [Direct String arrays and String-valued maps](string-arrays.md) are now supported. [String-array fields in records](record-string-array-fields.md) are also supported. Returning a field as a standalone String is supported.

String fields may coexist with all six implemented CPU numeric families, scalar Bool, numeric arrays/tensors, Bool arrays and finite nested records. GPU record placement, native distributed execution, whole-record CLI formatting and public C/C++ interfaces remain separate work. These are local CPU values, not a public binary or wire format.

The initial runtime revalidates text at helper and handoff boundaries. Eliminating redundant scans and reclaiming earlier temporaries belong to later optimization work; they do not change immutable value semantics.

## Qualification

The focused suite exercises nested updates, shared and empty fields, exact Unicode/NUL bytes, record arrays and maps, nested captures, imported helpers, six numeric families, repeated awaits, task recovery and managed diagnostic text. Private worker probes check source and scheduler settlement, malformed descriptors/UTF-8, partial retention failure, cancellation/deadlines, admission failure and allocation-failure cleanup. Rejected builds must preserve existing output files. The example above is compiled and executed in each build variant.

The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-string-fields-2026-10-18.json) lists the qualified checks, artifacts and scope.
