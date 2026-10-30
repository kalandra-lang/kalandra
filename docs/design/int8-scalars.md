# Checked CPU int8 and uint8 scalars

The native CPU scalar subset now supports `int8` (−128 through 127) and `uint8` (0 through 255). They remain distinct types; `int` and `uint` still mean int64 and uint64.

```kalandra
fn increment(value: uint8) -> uint8:
  value + 1

fn main() -> uint8 ! TaskError:
  try parallel:
    try await spawn increment(41)
```

This prints `42`. Functions, branches, comparisons, scalar captures, local modules and worker results keep the exact 8-bit identity. A literal can take its type from context, but already typed values do not silently narrow or mix signed and unsigned types.

## Checked arithmetic

Addition, subtraction, multiplication, division, remainder and unary arithmetic preserve the operand type. Out-of-range literals and known invalid arithmetic are compiler errors. Dynamic overflow and zero divisors produce checked Arithmetic faults in every build mode; workers expose those failures through `TaskError`.

Signed division truncates toward zero. `int8(-128) / int8(-1)` overflows, while its remainder is zero. Unsigned subtraction cannot go below zero, and unsigned negation accepts only zero. There is no implicit wrapping arithmetic.

The shared integer lowering checks widened intermediates before narrowing the result. Comparison operators use the operand's signedness and return Bool.

## Explicit conversions and metadata

Numeric conversion calls now cover all scalar pairs among int8, uint8, int32, uint32, int64, uint64, float32 and float64: the original 36 pairs plus 28 involving the two byte types. The subsequent [16-bit milestone](int16-scalars.md) extends scalar conversions to all 100 pairs among the ten accepted scalar types. Integer conversions preserve value or fail; floating conversions retain the existing rounding, truncation and nonfinite-value rules. For example, `int8(127.9)` is 127, while `uint8(-1)` is rejected. An annotation alone does not convert a typed value.

8-bit integer scalars can also supply existing integer index, slice-bound and reshape-dimension parameters. They extend according to signedness before the existing range checks; a negative index does not become a positive offset.

## Ownership, display and scope

Workers use distinct private layouts for signed and unsigned byte values, alongside the existing scalar and collection layouts. Nested calls, repeated awaits, mixed scalar captures, error recovery and task cleanup retain their existing rules. No public native ABI is introduced.

The command-line display adapter widens a completed signed byte with sign extension and an unsigned byte with zero extension, then uses the existing 64-bit display entry. Language computation and worker storage remain 8-bit; displaying −128 or 255 does not change the source type.

This milestone established CPU scalars. The subsequent [narrow array and record milestone](narrow-arrays.md) enables arrays and scalar/array record fields for these types. [Narrow CPU tensor construction and elementwise arithmetic](narrow-tensors.md) are also implemented. [Checked narrow CPU reductions](narrow-reductions.md) are supported. [Checked narrow CPU matrix multiplication](narrow-matmul.md) is supported. [Checked CPU tensor numeric conversions](narrow-tensor-conversions.md) support all ten element types. GPU arithmetic remains separately gated.

## Qualification

The focused tests exhaust all operand pairs for signed and unsigned 8-bit binary arithmetic and comparisons, and all unary inputs. Conversion tests cover all 28 new pairs against LLVM integer/APFloat references under eight ARM floating environments, exhaust byte source representations, and probe wider numeric edge/random inputs. Generated programs exercise nested/cached worker results, signed/unsigned/float layout isolation, cancellation, deadlines, admission and allocation failures, source locations, String/collection captures and source settlement. Both the compiler and checker accept the same project-based examples. The [evidence record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-int8-scalars-2026-10-18.json) records the final passing checks and scope.
