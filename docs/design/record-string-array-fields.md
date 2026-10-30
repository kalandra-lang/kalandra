# String-array fields in native CPU records

Records can contain immutable `[String]` fields alongside scalar Strings, numeric and Bool values, nested records, numeric arrays/tensors and Bool arrays.

```kalandra
type Names:
  values: [String]

fn decorate(names: Names) -> Names:
  names with {values: map(names.values, fn(text): text + "!")}

fn main() -> [String] ! TaskError:
  original = Names {values: ["north", "south"]}
  changed = try parallel:
    try await spawn decorate(original)
  [original.values[0], changed.values[0]]
```

This prints `["north", "north!"]`. The update creates a new record and array; the original values remain unchanged. Empty arrays have field type context, so `Names {values: []}` works too.

Fields support checked indexing, `length`, conditionals and named or anonymous maps. Callbacks can capture whole records, including nested String-array fields. Arrays of these records can be mapped and passed through local CPU workers. Imported functions can construct and return records using private nominal types, under the existing module rules.

Worker transfer retains both the array's descriptor storage and every String's storage. Sharing an array across fields, sharing text across cells, or also storing that text in a scalar field preserves immutable ownership. The source invocation and scheduler can settle before an observed result is released. Nested tasks and repeated awaits preserve these guarantees.

Every transferred field is validated, including each UTF-8 String and the surrounding array bounds. Invalid descriptors, cancellation, deadlines, allocation failures and partial retention settle their acquired owners without publishing a partial record. This is local CPU execution; it introduces no distributed transport.

The inline record layout counts each String-array descriptor as five storage units, independently of its element count. Existing finite-layout and cycle checks still apply. Nested arrays, String tensors, record-array fields, GPU Strings and whole-record CLI display remain outside this feature. A standalone `[String]` result uses the existing escaped array display.

Qualification covers debug, optimized, ASan/UBSan and TSan generated programs, nested updates, empty fields, record arrays/maps, captures, numeric/Bool siblings, imported helpers, error recovery, malformed inputs and allocation failure. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-record-string-array-fields-2026-10-18.json) records the checks and preserved numerical artifacts.
