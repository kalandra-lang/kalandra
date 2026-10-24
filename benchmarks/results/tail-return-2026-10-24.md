# Scalar tail-return and signed-remainder follow-up

Token-bound GCD takes **20.2% less time** at the median of nine adjacent before/after blocks. All nine blocks improve. The default unbound GCD change is small and noisy; this step does **not** establish the Rust/C++ 10% target. Polynomial recursion is effectively unchanged. All source algorithms, numeric policies and safety limits remain intact.

## Matched controls

The baseline is the compiler from `2585de04`. Both compilers lower identical source and link the same current optimized runtime archive with Clang -O2 and separately rounded arithmetic. Normal invocation lifecycle and token/counter binding are included. Every warmup/measured checksum matches an independent integer/Fraction/GCD reference. Depth 14 gives 16,384 leaves.

Nine blocks per algorithm/mode alternate adjacent ABBA and BAAB fresh processes, use identical iteration counts within each pair, and run two warmups plus three measured batches per process. The table reports the median of the per-block time reductions; every block and process sample is retained.

| Algorithm | Invocation | Median paired time reduction | Range across nine blocks |
| --- | --- | ---: | ---: |
| polynomial | unbound | -0.1% | -168.1% to 7.9% |
| polynomial | bound | 0.4% | -29.1% to 24.2% |
| polynomial | observed | -0.0% | -3.5% to 4.8% |
| branches | unbound | -0.0% | -24.2% to 31.2% |
| branches | bound | 2.6% | -11.7% to 9.2% |
| branches | observed | -0.2% | -41.9% to 7.9% |
| euclid | unbound | 2.9% | -33.8% to 26.8% |
| euclid | bound | 20.2% | 19.0% to 50.6% |
| euclid | observed | -0.1% | -23.3% to 11.2% |

Positive reductions mean less time. Concurrent external Arbiter/Xcode compilation causes large outliers, including the observed controls. Unbound GCD has a 2.9% median reduction with a wide range; that is not a precise performance claim. Bound GCD improves in all nine blocks (19.0–50.6%), with medians of block times around 705 → 566 µs. The roughly 20% result applies to this token-bound workload, not general recursion or a language-wide score.

[Paired raw data, commands and hashes](tail-return-paired-2026-10-24.json). The earlier [randomly interleaved control](tail-return-controls-2026-10-24.json) is also retained; its scheduling variance makes some medians misleading. The [fresh five-language run](fair-source-tail-return-2026-10-24.md) passes all 180 reference workloads and all timed outputs, but its absolute rankings are unreliable under the same contention. No parity claim is made from those rankings.

## Implementation and semantic boundaries

- Signed int64 remainder maps a divisor of -1 to 1. Both remainders are zero for every dividend, so this removes the INT64_MIN comparison and conjunction while avoiding LLVM poison. Zero still faults at its original location; division overflow remains a fault.
- A proven closed scalar tail body in unobserved token mode carries the return count and sticky cancellation result in SSA. Every original acquire token poll executes. No source operation, callback, observer or fault creation occurs inside that loop; final depth and lease restoration are batched. Faulted/observed invocations retain checked unwinding.
- The optional return expansion requires 1 KiB of extra conservative headroom; the final 16 KiB function-budget check remains. Logical depth stays 128 and native stack reserve stays 128 KiB. No runtime ABI change, FMA, reassociation or benchmark algorithm change is introduced.

## Verification

- **3,084,456 current-side differential/reference cases** exactly match the pushed compiler: 716,496 recursive integer/float32/float64/state cases across both arithmetic policies, plus 2,367,960 signed-remainder/state cases. Debug, optimized, ASan/UBSan and TSan variants pass.
- The remainder reference exhausts the signed 8-bit domain in int64 code, including zero divisors; full-width limits, deterministic random values and -1 divisors follow. Six invocation modes cover host admissions, observed counters, cancellation and fault priority.
- Recursive checks retain exact result bits, fault codes/locations, poll counts and final depth, plus asynchronous cancellation, worker migration, low native stacks and non-fused rounding witnesses. Structural checks require an acquire load and no runtime callback or depth store inside the private token-return poll block.
- **540 machine-frame records** across 36 binaries remain within budget; the maximum is **1,200 bytes**.
- **38 unique CTest groups passed across the main run and affected reruns**, including int64 scalars, tasks, tensors, reductions, matrix products, arithmetic policies, benchmark-cell correctness, CPU numerical optimization, tail/fast calls and structural checks. Four paired floating source configurations are recorded separately.
- All 39 native documentation snippets across 20 pages/63 rendered blocks are checked: 32 CPU programs, two physical Metal programs, four companion modules and one expected diagnostic. Workflow formatting and all 28 release/provisioning unit tests pass.

[Qualification records and retained logs](tail-return-validation-2026-10-24.json), [machine frames](tail-return-stack-audit-2026-10-24.json), and [semantic invariants](../../docs/design/checked-call-optimization.md).

## Qualification repairs

The main 32-group run passed 30 groups. Two old int64 frontend assertions still expected GPU numeric support to be rejected. The semantic checker is unchanged; the completed GPU numeric model already supports these types. The int64/uint64/uint32 assertions now require the supported capability, and all six affected frontend variants pass. The original failed log is retained.

The preceding checkpoint passed hosted Windows qualification. Its Linux build caught an LLVM RTTI ABI mismatch: the official archives omit CallbackVH typeinfo. The backend now matches LLVM_ENABLE_RTTI. Local compilation without RTTI passes and llvm-nm confirms the missing typeinfo reference is gone; hosted Linux execution of this fix remains pending. Linux/macOS workflows now run the focused recursive/remainder qualification. Downloaded Linux/Windows acceptance gains a signed-remainder/GCD case, also compiled and executed locally in all four modes.

The first documentation attempt lacked sandbox GPU device access and was repeated with physical Metal access; the final exact compiler passes both GPU examples.

## Rejected experiment and remaining work

The [general non-tail depth-cache experiment](recursive-depth-cache-experiment-2026-10-24.json), with its [source patch](recursive-depth-cache-experiment-2026-10-24.patch.txt), is not included in production. It added complexity without a reliable gain under the competing build load. Its raw results are retained without a speedup claim.

The default recursive path still pays for logical-depth memory traffic, physical admission, checked scalar operations and status propagation. Further register-carried state or range analysis needs a separate proof, a quiet measurement window and the same differential checks. The Rust/C++ target remains unmet; this follow-up makes no GPU-recursion timing claim.
