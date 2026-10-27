# Ordinary entry ABI and private buffer lifetime — 2026-10-27

Both complete ordinary comparisons place all eight runtime-input cases within
10% of the faster C++/Rust reference. Arrays are faster in every case. The scalar
zero-round ordering changes between repeats at these tiny durations; this is not
a claim of universally beating Rust. In the first run it is 4.85% slower; in the
repeat it is 6.47% faster. The worst repeat gap is 3.13% at scalar 4096 rounds.

This study compares a compiler working tree based on `437a9ce7` with that exact
frozen compiler and its matching runtime. The baseline compiler hash matches the
previous [qualification](entry-costs-qualification-2026-10-27.json). Measurements
ran on Apple M2 Max, macOS 27.2, deployment target macOS 26. Performance on a
physical macOS 26 installation and Windows/Linux is not established here.

## Implementation and boundaries

The compiler can emit a private ordinary scalar companion with a 24-byte fault
output instead of constructing an Invocation. It proves the final closed graph
has no task/foreign effects, caller pointers, escaping resources, growing calls,
volatile/atomic accesses or external memory. All arithmetic and first-fault
checks remain. Integer high-bit-only values (true for Bool) and floating NaNs
consult the fault output; successful ambiguous values retain every bit and clear
the code. No numeric representation is reserved. Return and parameter extension
attributes survive; old result range/NaN refinements are discarded.

Constant-size private numeric allocations can use four aligned 128-byte stack
slots. This is an optimization capacity, not a language allocation limit. Pool
exhaustion, an unrepresentable release or injected allocation failure replays the
original checked graph from its scalar arguments with a real Invocation. Private
stack pointers never escape or enter replay. Unsupported graphs retain the checked
entry. The allocator, ownership ledger and Invocation layout remain unchanged.

A general zero-argument specialization is eligible only when it eliminates every
loop and reduces the whole body to at most 32 instructions and one third of its
original size. It retains checks that precede the base return; other inputs
musttail-transfer to the complete ordinary worker. It excludes buffered entries
and uses no workload names or fixed round ranges. The proof unit checks generic
versioning, ABI attributes, rejection cases and idempotence.

The CLI uses eligible companions for scalar mains, so this is a compiler/runtime
feature rather than only a timing-harness shortcut. Emitted library IR retains
checked entries. Task/cancellable execution keeps its existing cancellation,
physical-stack admission and settlement. Mixed CPU/Metal and unsupported source
graphs retain their established execution paths. This is not a GPU speed claim.

See [design details](../../docs/design/checked-call-optimization.md).

## Fair ordinary comparison

The eight-element immutable array map and repeatedly applied captured scalar
closure use unchanged source algorithms and unchanged C++/Rust references. No
numerical library or algorithm substitution was introduced. All use the same
external C++ timing/validation driver, separate objects and no LTO. The new
`--ordinary-entry` option selects a companion only when emitted; manifests record
`ordinary-v1` versus `checked-v4` per program. Both before/after executions are
ordinary, with no cancellation token or native-interrupt capability supplied.
Fixed-input task diagnostics retain checked entries and are not language rankings.

Durations are ns/call. The last columns compare Kalandra with the faster reference
in the same complete run; negative values mean faster.

| Program | Rounds | Kalandra | C++ | Rust | Gap, run 1 | Gap, repeat |
|---|---:|---:|---:|---:|---:|---:|
| arrays | 0 | 6.227 | 21.592 | 22.177 | -71.16% | -70.49% |
| arrays | 16 | 125.921 | 472.058 | 401.400 | -68.63% | -70.13% |
| arrays | 64 | 481.546 | 1862.078 | 1727.395 | -72.12% | -71.93% |
| arrays | 256 | 1896.804 | 7682.272 | 7171.429 | -73.55% | -73.55% |
| scalar | 0 | 1.488 | 4.058 | 1.419 | +4.85% | -6.47% |
| scalar | 16 | 23.323 | 27.712 | 27.562 | -15.38% | -17.49% |
| scalar | 256 | 900.018 | 907.408 | 906.938 | -0.76% | -0.45% |
| scalar | 4096 | 15082.337 | 15025.579 | 15194.044 | +0.38% | +3.13% |

The [first run](ordinary-abi-runtime-2026-10-27.json),
[independent repeat](ordinary-abi-runtime-repeat-2026-10-27.json), and
[cancellable run](ordinary-abi-cancellable-2026-10-27.json) retain every observation,
command, source snapshot, oracle and artifact hash. Their independent
[closed-form modular audit](ordinary-abi-runtime-audit-2026-10-27.json) verifies
all 297 verification workloads and 2,673 timed/warmup samples. The cancellable
run remains a separate contract and is not used to claim ordinary parity.

## Paired before/after comparison

Nine adjacent ABBA/BAAB process blocks per group use identical iteration counts
within each pair. Source algorithm, reference and driver hashes must match.
The [612 measured processes](ordinary-abi-paired-2026-10-27.json) and
[independent audit](ordinary-abi-paired-audit-2026-10-27.json) include separate
whole-task diagnostics. No build, profiler or test ran during acceptance timing.
CPU frequency, affinity and desktop activity were not controlled.

The first two columns of timings are medians of process medians. The final
column is the median paired time change, which need not equal their ratio.

| Program | Rounds | Previous ns | Current ns | Paired time change |
|---|---:|---:|---:|---:|
| arrays | 0 | 42.260 | 6.486 | -84.41% |
| arrays | 16 | 341.311 | 122.776 | -64.03% |
| arrays | 64 | 1057.580 | 474.117 | -54.95% |
| arrays | 256 | 3873.264 | 1867.111 | -51.87% |
| closure | 0 | 2.497 | 1.354 | -45.66% |
| closure | 16 | 21.998 | 22.310 | +0.52% |
| closure | 256 | 907.606 | 902.541 | -0.33% |
| closure | 4096 | 15099.230 | 15036.290 | -0.75% |

The small scalar 16-round regression is retained and visible; its complete-run
results still beat both references. Ordinary parity is demonstrated for these
eight cases on this host, not for the whole language or every workload. Further
work should widen the fair workload corpus and reduce residual scalar arithmetic
and dispatch cost without discarding checked semantics.

## Final qualification

The final compiler passed all **423 selected CPU regressions and 50 selected
Metal tests**, with no failures or skips. The full native build completed.
The new proof unit covers ten effect/fault/ABI cases, a general loop-free
base-case version with matching tail-call attributes, and idempotence.

The ordinary scalar source matrix checks **1,744 exact/first-fault/payload cases**
across debug, optimized, ASan/UBSan and TSan builds: all ten numeric types and
Bool, signed/unsigned boundaries, signed zeros, subnormals, infinities, quiet and
signaling NaN payloads, plus arithmetic overflow projected to float32/float64/Bool.
The private-buffer matrix checks **6,912 cases** in the same four build modes,
including aliases, extreme inputs, zero/nonempty work, pool exhaustion/replay and
eight allocation-failure settings. Original checked entries and independent
integer/bit oracles supply the references.

[Qualification manifest](ordinary-abi-qualification-2026-10-27.json),
[CPU log](ordinary-abi-cpu-2026-10-27.log),
[Metal log](ordinary-abi-metal-2026-10-27.log). Correctness tests ran after
performance measurements. Host minimum-version and cross-platform limitations
above still apply.
