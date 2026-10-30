# CPU Boolean values and scalar comparisons

The native CPU subset supports immutable `Bool` values, the reserved literals `true` and `false`, and scalar comparisons. Bool is a separate built-in type, not a numeric alias. It can be a parameter, binding, inferred/declared return value, worker capture or completed worker result. A Bool `main` prints `true` or `false`.

```kalandra
fn aboveLimit(value: float64, limit: float64) -> Bool:
  value > limit

fn main() -> Bool ! TaskError:
  try parallel:
    work = spawn aboveLimit(12.5, 10.0)
    try await work
```

## Operators and typing

All six implemented numeric types—int32, uint32, int64, uint64, float32 and float64—support `==`, `!=`, `<`, `<=`, `>` and `>=`. Both operands must have the same concrete type. Numeric literals can take the other operand's type; two unconstrained numeric literals follow the normal int64/float64 defaults. When both operands are literal expressions and either is floating, both receive float64 context. The comparison's Bool result context does not change operand precision.

Bool supports `==` and `!=`; ordering Booleans is rejected. Bool does not participate in arithmetic, numeric conversions, numeric truthiness or implicit conversion to/from zero and one. Aliases retain their existing numeric meanings. Tensors, task handles, unobserved expected-error results and device-resident values cannot be scalar comparison operands. Tensor comparisons will need the accepted elementwise Bool-tensor implementation separately.

Integer comparisons use mathematical signed or unsigned ordering at their declared widths. Floating comparisons follow the accepted [numeric contract](numbers.md#floating-point-arithmetic): NaN is unequal to every value, including itself, and all ordered comparisons involving NaN are false. Positive and negative zero compare equal. Infinities retain their usual ordering. Comparisons do not canonicalize or mutate their inputs, raise ArithmeticFault for NaN, or trigger floating exception flags, including for signaling NaNs. The CPU lowering compares integer representations so results do not depend on ARM rounding, flush-to-zero or default-NaN settings.

## Grouping and evaluation

Precedence, from tighter to looser, is calls/unary signs, multiplicative arithmetic, additive arithmetic, pipelines, comparisons, Boolean not/and/or, then catch. Thus `1 + 2 -> twice == 6` means `(twice(1 + 2)) == 6`. This extends the existing arithmetic and pipeline rules without changing their relative order.

A comparison evaluates its left operand, then its right operand, once each. If the left fails, the right does not execute. Checked failures in either operand keep their own source location; comparing them does not suppress faults.

Ungrouped comparison chains such as `a < b < c` are rejected with P034. Explicit grouping is permitted when types agree: `(a < b) == true` compares two Booleans. This does not define Python-style chains or repeat a middle operand.

`true` and `false` cannot be function, parameter, local, handler or pattern binding names. `Bool` is reserved as a function name and is not a conversion function. Invalid operand/type uses are diagnosed before executable publication; there is no dynamic fallback.

## Tasks and current limits

Boolean captures and results use the existing bounded scheduler, resumable calls, cached awaits, expected-error handling and lifetime rules. Bool has a distinct private native result-layout identity; task handles retain their own representation. Rejected layouts must not publish a value or acquire owners. Ordinary CPU functions and checked private native test interfaces carry Boolean scalars directly. This does not establish a public C ABI or library-packaging contract.

[CPU conditionals and short-circuit logic](control-flow.md) now build on this foundation. [Immutable CPU Bool arrays](bool-arrays.md) now support construction, indexing, mapping and retained worker transfer. Bool tensors and GPU/mixed-target Bool execution remain subsequent work. Broader values/modules, native library embedding and developer tooling remain tracked in the [roadmap](../roadmap.md). Native distributed execution still requires the agreed design discussion.

## Qualification

The [frontend suite](../../native/tests/BooleanTests.cpp) checks all 36 numeric type/operator combinations, Boolean literals and equality, grouping and pipeline precedence, reserved names, strict numeric/task boundaries and the GPU gate. The [compiled driver](../../native/cmake/VerifyBooleans.cmake) and [runtime harness](../../native/tests/BooleanRuntimeTests.cpp) compare exact Boolean results with independent wide-integer/APFloat ordering across boundary, exponent-stratified and deterministic random inputs, under eight ARM floating environments. LLVM artifacts must contain no hardware `fcmp` instructions for these probes.

The [worker harness](../../native/tests/BooleanTaskTests.cpp) checks both Boolean outcomes, mixed numeric/Boolean packets, nested/cached awaits, recovery using owned tensors, source-located operand faults, layout rejection, allocation injection, cancellation, deadlines, admission and settled owners with one, two and four workers. The sanitized frontend target links the instrumented frontend/semantic libraries independently from the uninstrumented MLIR backend; generated runtime programs are qualified separately in debug, optimized, ASan/UBSan and TSan builds.

The [qualification record](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/cpu-booleans-2026-10-17.json) combines 79 distinct passing focused checks, including the corrected full Boolean suites in debug, optimized, ASan/UBSan and TSan builds. Numerical probes verify 125,781,504 exact comparison results; worker probes run 984 harness processes. All 640 tracked float32 numerical artifacts remain unchanged, as do the arithmetic/conversion engines, scheduler and storage runtime. This is focused qualification rather than the full 343-test suite.
