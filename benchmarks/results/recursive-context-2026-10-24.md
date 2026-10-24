# Recursive call-chain context optimization

The final compiler reduces unbound recursive source-workload time by **17–29%** in the interleaved before/after control, with **18–26%** reductions when a cancellation token is bound. Observed controls differ by less than 1% in this run. The Rust/C++ 10% target remains unmet. Every source algorithm and arithmetic policy is unchanged.

## Matched compiler comparison

The baseline compiler was frozen from the code committed as `bd41ea8e`; its hash matches the preceding inlining report. Both versions compile identical source, use identical Clang flags and link the **same current runtime archive**. The archive was rebuilt and differs from the prior recorded archive, while Runtime.cpp/Runtime.h source hashes are unchanged. This comparison isolates the compiler change using one runtime on both sides.

Eighteen cells interleave baseline/current and clear/bound/observed invocations: three fresh processes, five measured and two warmup batches each. Depth 14 means 16,384 leaves. Binding, normal invocation lifecycle and observer counter initialization are included. Every checksum matches an independent integer/Fraction/GCD reference. Observed counters preserve the ordinary checked execution path.

| Algorithm | Invocation | Before µs | After µs | Time reduction | Speedup |
| --- | --- | ---: | ---: | ---: | ---: |
| polynomial | unbound | 99.600 | 70.323 | 29.4% | 1.416× |
| polynomial | bound | 95.718 | 70.490 | 26.4% | 1.358× |
| polynomial | observed | 159.435 | 160.261 | -0.5% | 0.995× |
| branches | unbound | 96.963 | 75.402 | 22.2% | 1.286× |
| branches | bound | 98.709 | 75.640 | 23.4% | 1.305× |
| branches | observed | 159.291 | 160.433 | -0.7% | 0.993× |
| euclid | unbound | 507.737 | 420.945 | 17.1% | 1.206× |
| euclid | bound | 818.277 | 669.904 | 18.1% | 1.221× |
| euclid | observed | 1319.148 | 1310.824 | 0.6% | 1.006× |

[Raw final controls, commands and hashes](recursive-context-final-controls-2026-10-24.json). CPU frequency/temperature is not pinned; sub-percent differences are not treated as gains.

## Equivalent source comparison

All five languages retain the same recursive traversal and leaf computations. No tensor built-ins or numerical-library calls substitute for source algorithms. Kalandra retains checked arithmetic, invocation, depth and cancellation handling; the other languages retain their normal semantics. All 180 edge/reference workloads and measured/warmup outputs pass.

| Algorithm / 16,384 leaves | Kalandra µs | C++ µs | Rust µs | Go µs | Python µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| polynomial | 69.479 | 38.840 | 40.864 | 34.997 | 2087.092 |
| branches | 75.106 | 48.708 | 48.784 | 51.838 | 2355.758 |
| euclid | 417.756 | 120.818 | 152.280 | 159.016 | 6916.662 |

[Full depth-10/depth-14 report](fair-source-recursive-context-2026-10-24.md) and [raw samples](fair-source-recursive-context-2026-10-24.json). The current gap is about **1.5–3.5× C++** and **1.5–2.8× Rust** on these narrow workloads. General recursion still has no supported GPU lowering; this work makes no new GPU timing claim.

## What changed

The compiler proves a closed scalar call graph and builds bounded private clear/bound-token versions. Admitted callers reuse the established call-chain context instead of repeatedly testing ownership of the outer stack lease. Pure tail versions specialize initial mode/token choices while retaining logical tail-depth accounting. Observed versions keep the original checked body and direct recursive edges. Public function identity/signature remains unchanged.

Every logical admission and return remains balanced. Token-bound paths retain acquire loads at the same logical points; fault paths retain ordinary priority/location handling. Every actual private physical frame checks cached native bounds on macOS. Already expanded logical calls in that fixed frame retain depth/poll checks without redundant physical checks; dominance and absence of frame allocations are verified. The outer public frame releases its own lease before migration.

Loops qualify only through the existing pure-tail proof. Scalar candidates are capped at 750 instructions with 1 KiB of reserved headroom; every version must fit the unchanged 16 KiB model. Inter-version token arguments are charged. A dispatcher must fit a 512-byte allowance, charged at every public expansion site. Large callers retain their original calls. Logical depth remains 128 and native reserve remains 128 KiB.

[Detailed invariants and exclusions](../../docs/design/checked-call-optimization.md#recursive-call-chain-contexts). Code growth is bounded to two specialized bodies plus an observed copy per admitted candidate; this is a runtime-speed/code-size tradeoff.

## Correctness and safety qualification

- **716,496 differential/reference cases passed**, paired with the pushed compiler across hardware/software arithmetic and debug, optimized, ASan/UBSan and TSan modes. Exact values, fault codes/locations, observed poll counts and final depth match. Each record captures compiler SHA-256, and the driver rejects a compiler changing during qualification.
- Float32/float64 cases include signed zero, subnormals, infinities/NaNs, integer conversion/division faults and a recursive multiply/subtract rounding witness. The independent reference proves that witness differs from FMA; generated LLVM rejects FMA/contract/reassociation flags.
- Integer/tail cases retain host admissions, depth exhaustion, mixed helper/non-tail fallbacks, fault priority, settled contexts, immediate/asynchronous cancellation, worker migration and low native stacks.
- **532 generated function/build/policy frame records**, including public and clear/token/observed versions, stay within the unchanged 16 KiB budget; the audited maximum is **1,200 bytes** across 32 binaries. Near-reserve execution additionally leaves about 1 KiB above the reserved boundary before deep clear/bound/observed recursion.
- **99 unique regression/qualification groups passed across the primary suite and affected reruns**, including worker/callable/record/string/collection ownership, continuations, parallel maps, conversions, source stack guards, physical Metal checks, static numerical shapes, installed/relocated packages and APFloat total casts. The initial failed run and all repairs are retained below.
- APFloat qualification passes **1,389,332 scalar cases** and **225,440 tensor-lane comparisons**, with 68 scalar and 48 ranked tensor records. All runnable documentation examples pass, including 32 CPU and two physical Metal programs across 20 pages/63 rendered blocks.
- Structural tests cover transitive exclusion through a memory-reading helper, reversed module order, preserved observed edges, acquire loads, physical/expanded logical gates and dispatcher-budget fallback.

[Qualification logs, records and source/compiler hashes](recursive-context-validation-2026-10-24.json) and [machine-frame audit](recursive-context-stack-audit-2026-10-24.json). The final compiler was rebuilt during a test-only packaging configuration change; differential and documentation qualification were refreshed for that exact binary.

## Validation repairs retained

The primary 96-group run passed 95 groups. One active-cancellation conversion control failed because its sender never requested cancellation before the entire kernel completed. The affected conversion LLVM is byte-identical to the preceding compiler. The harness now validates normal completion/cleanup and retries a larger workload only when no request was issued. Publication after an actual request still fails, and eight missed attempts fail the control. All affected tensor groups were rerun successfully; the initial failure remains in the qualification JSON.

The preceding hosted checkpoint passed Linux/Windows qualification but failed macOS package acceptance because its numeric GPU fixture intentionally converts every result to float64 without opting into unsupported-hardware fallback. That fixture now explicitly selects hardware-with-software-fallback; hardware float32 arithmetic remains enabled. Local installed/relocated Metal qualification also uses the already configured, verified Apple compiler because this Mac’s default Xcode metal shim cannot locate its optional component. Both Metal package gates now pass locally. No default fallback or compiler-version check was weakened. New hosted qualification for this source is still pending.

## Investigation and retained intermediate results

The [diagnostic ablation](recursive-context-profile-2026-10-24.json), with its [exact script snapshot](recursive-context-profile-script-2026-10-24.txt), isolated repeated stack admission and polling costs. Those ignored LLVM copies disable protections and are invalid language implementations. Some compiler-generated tail-depth logic remains in the ABI-guard ablation; the variants are not additive and do not establish an unguarded performance score.

The [initial context trial](recursive-context-trial-2026-10-24.json), [fixed-frame trial](recursive-context-frame-trial-2026-10-24.json), and [tail/context trial before the observed copy](recursive-context-controls-2026-10-24.json) remain separate historical measurements. The latter exposed an approximately 4% observed polynomial/branch regression; the final observed copy removes repeated dispatch on those recursive edges. Only the final records qualify the current implementation.

## Next bottleneck

The generated recursive path still loads/stores logical depth and performs checked scalar arithmetic. GCD also retains signed remainder edge handling, depth accounting and token-return polling. A further pass should profile these final bodies and investigate carrying proven state in registers or deriving safe arithmetic ranges. Removing guards, changing source algorithms or using tensor kernels would not satisfy the requested source-performance target.
