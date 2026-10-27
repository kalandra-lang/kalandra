# Direct buffer entries and Windows ABI qualification

Baseline source: `fd49474d`; candidate compiler/runtime source: `f6bdb5dc`. The qualification-only follow-ups `a379423e` and `fabfc6fd` add the missing hosted optimized-runtime dependency and locate Windows sanitizer DLLs. Compiler/runtime and benchmark source bytes remain unchanged. Timing ran on the same Apple M2 Max/macOS 27.2 host with frozen binaries, after all builds/tests stopped. This is an ordinary standalone CPU comparison, with identical source-level algorithms, strict floating arithmetic and independent result oracles. It is not a universal language ranking or physical macOS 26 qualification.

All **32/32** cells remain within 10% of the faster C++/Rust median in both candidate runs. Kalandra has the lower median in **26/32**, then **25/32** in the repeat. The eight legacy array/closure controls also meet the 10% threshold. None of these measurements changes the checked task/cancellation contract.

## Result

Short 8- and 16-element buffers improve about **6–11%** versus the published compiler in the nine-block paired study. Most longer workloads change little. The 32-element zero-round case regresses **7.7%**, and 128-element zero-round regresses **3.2%**; these losses are retained below. Both still beat C++/Rust. Removing status transport does not guarantee a faster instruction layout for every case.

The scalar-only affine/polynomial/Euclid paired controls change by less than 1%. All five buffer program sizes now select `ordinary-direct-v1`, replacing recoverable `ordinary-v1`. The legacy arrays control also changes to the direct ABI; its scalar closure already had that ABI in the baseline.

| Program | Rounds | Kalandra ns | C++ ns | Rust ns | Vs faster reference | Repeat vs reference | Paired time reduction vs baseline |
|---|---:|---:|---:|---:|---:|---:|---:|
| affine | 0 | 0.909 | 1.175 | 1.168 | -22.2% | -23.8% | -0.1% |
| affine | 1 | 0.907 | 0.898 | 0.908 | +1.0% | +0.6% | -0.5% |
| affine | 16 | 4.225 | 7.763 | 7.560 | -44.1% | -42.6% | +0.5% |
| affine | 256 | 366.514 | 377.903 | 361.549 | +1.4% | +1.8% | +0.6% |
| polynomial | 0 | 0.948 | 1.188 | 1.156 | -17.9% | -18.7% | +0.3% |
| polynomial | 1 | 0.948 | 0.901 | 0.902 | +5.2% | +6.3% | +0.0% |
| polynomial | 16 | 4.827 | 7.827 | 7.823 | -38.3% | -35.6% | +0.2% |
| polynomial | 256 | 367.565 | 367.428 | 369.393 | +0.0% | +0.4% | +0.0% |
| euclid | 0 | 1.159 | 1.184 | 1.206 | -2.1% | +0.7% | -0.4% |
| euclid | 1 | 2.969 | 3.303 | 3.939 | -10.1% | -7.7% | +0.9% |
| euclid | 16 | 160.216 | 156.058 | 155.990 | +2.7% | +3.3% | +0.2% |
| euclid | 256 | 3175.878 | 3208.642 | 3158.598 | +0.5% | +2.8% | +0.1% |
| array8 | 0 | 6.181 | 23.037 | 22.589 | -72.6% | -70.3% | +11.0% |
| array8 | 1 | 10.783 | 52.171 | 46.393 | -76.8% | -76.2% | +8.8% |
| array8 | 16 | 125.168 | 469.442 | 402.666 | -68.9% | -69.8% | +2.5% |
| array8 | 256 | 1932.501 | 7740.397 | 7235.660 | -73.3% | -73.5% | +3.0% |
| array16 | 0 | 12.347 | 25.905 | 24.458 | -49.5% | -49.3% | +9.3% |
| array16 | 1 | 27.473 | 62.767 | 54.986 | -50.0% | -51.1% | +5.8% |
| array16 | 16 | 259.533 | 584.992 | 513.488 | -49.5% | -49.2% | +0.7% |
| array16 | 256 | 3904.340 | 9573.975 | 8739.380 | -55.3% | -56.1% | +0.4% |
| array32 | 0 | 10.172 | 28.749 | 26.149 | -61.1% | -61.3% | -7.7% |
| array32 | 1 | 34.705 | 72.893 | 72.561 | -52.2% | -51.8% | +1.7% |
| array32 | 16 | 479.312 | 743.821 | 707.280 | -32.2% | -28.1% | -0.3% |
| array32 | 256 | 7509.796 | 11922.653 | 11715.659 | -35.9% | -33.4% | -0.5% |
| array64 | 0 | 21.221 | 40.455 | 31.193 | -32.0% | -32.5% | -1.5% |
| array64 | 1 | 80.702 | 107.374 | 99.889 | -19.2% | -21.3% | +0.5% |
| array64 | 16 | 940.564 | 1145.959 | 1110.789 | -15.3% | -16.6% | -0.2% |
| array64 | 256 | 14693.664 | 18341.536 | 17811.067 | -17.5% | -16.7% | +0.6% |
| array128 | 0 | 50.579 | 62.008 | 53.075 | -4.7% | -6.0% | -3.2% |
| array128 | 1 | 156.138 | 187.265 | 166.730 | -6.4% | -6.1% | +0.6% |
| array128 | 16 | 1806.240 | 2039.057 | 1944.169 | -7.1% | -7.1% | -0.8% |
| array128 | 256 | 28174.253 | 34713.559 | 32438.625 | -13.1% | -14.2% | -0.5% |

Negative reference differences mean less time; positive paired reductions mean improvement. Each full run uses three independent process medians and adaptive warmed batches. The paired study uses nine adjacent ABBA/BAAB process blocks and equal counts within each pair. Every warmup and measured return is validated. Desktop activity, CPU affinity and frequency were not isolated.

## Ownership and ABI

Buffer-owning direct companions accept scalar arguments and return a scalar. Their private bounded pools cannot escape. Generated workers and heap replay retain identical signatures and mandatory tail transfers; they are not flattened into larger frames. The conservative 16 KiB admission remains in force. Heap replay constructs a fresh invocation, executes the checked computation and settles it before terminal failure reporting. Recoverable exports and task/GPU entry selection remain available.

Windows keeps the two-word value/error helper internal, exposing only a scalar C ABI. Extension attributes survive for narrow signed/unsigned integers and Bool. A scoped zero-code LLVM intrinsic prevents LLVM 23/AArch64 from inserting conversions after narrow unsigned mandatory tail calls; optimization remains enabled. See [the design](../../docs/design/checked-call-optimization.md#prove-an-ordinary-entrys-resource-and-fault-abi).

## Evidence

[Fresh baseline](buffer-direct-baseline-2026-10-27.json), [candidate](buffer-direct-current-2026-10-27.json), [repeat](buffer-direct-repeat-2026-10-27.json), [paired study](buffer-direct-paired-2026-10-27.json), [independent broad audit](buffer-direct-audit-2026-10-27.json).

[Legacy controls](buffer-direct-controls-2026-10-27.json), [paired controls](buffer-direct-controls-paired-2026-10-27.json), [closed-form oracle audit](buffer-direct-controls-audit-2026-10-27.json), [paired audit](buffer-direct-controls-paired-audit-2026-10-27.json).

Final qualification passed **432 selected CPU regressions** (9 focused plus 423 broader tests) and **50 selected real-device Metal tests**, with no failures or skips. Exact matrices include 16,896 buffer/alias/replay cases, 1,744 typed buffer ABI cases, 1,744 scalar ABI cases, 4,724,480 strict-loop cases and 6,294,528 integer-domain cases. The buffer terminal suite matches 1,536 direct/recoverable process outcomes; scalar terminal checks cover 528 outcomes plus 48 CLI comparisons. A separate 48-case standalone buffer CLI audit covers allocation replay, index/arithmetic failures and ambiguous valid values. Mathematical unit tests also cover 262,144 exhaustive values.

Actual hosted Windows execution passed debug, optimized and ASan/UBSan matrices for all ten numeric types and Bool, buffers, loop proofs and terminal/recoverable cleanup. Windows does not have a TSan mode. Linux and Windows independently reproduced and qualified downloaded standalone packages. macOS signing, notarization, stapling and assessment passed for the compiler change. A later Windows-test-harness-only macOS workflow is still running; it changes no compiler/runtime code.

[Qualification manifest and frozen hashes](buffer-direct-qualification-2026-10-27.json), [numerical records](buffer-direct-numerical-qualification-2026-10-27.json), [Windows execution records](buffer-direct-windows-qualification-2026-10-27.json), [focused tests](buffer-direct-focused-qualification-2026-10-27.log), [CPU log](buffer-direct-cpu-qualification-2026-10-27.log), [CPU JUnit](buffer-direct-cpu-qualification-2026-10-27.xml), [Metal log](buffer-direct-metal-qualification-2026-10-27.log), [Metal JUnit](buffer-direct-metal-qualification-2026-10-27.xml), [full build](buffer-direct-build-qualification-2026-10-27.log).

[Windows package qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40599279121), [Linux package qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40599279104), [compiler-source signing/notarization](https://github.com/kalandra-lang/kalandra/actions/runs/40596994015), [later harness-only macOS rerun](https://github.com/kalandra-lang/kalandra/actions/runs/40599279119).
