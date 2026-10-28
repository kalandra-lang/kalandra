# Bounded private-buffer reduction folding

Baseline compiler/runtime source: `f6bdb5dc` (published evidence `32689919`); final compiler and tests: `1691341d233e48675890fab6fc310e9a78ad5942`. This study ran on Apple M2 Max/macOS 27.2 using frozen binaries after local builds and tests stopped. It compares equivalent source-level algorithms under the ordinary standalone CPU contract. No vendor numerical library, fast floating arithmetic or benchmark-specific recognition was added.

The compiler now lets LLVM eliminate bounded constant buffer construction, copying and reduction when the entire candidate becomes compact scalar work. The general nonempty program still uses its vectorized implementation. Zero-round 32-, 64- and 128-element programs take **79.0%, 88.5% and 95.9% less time** in nine-block paired measurements. The 128-element result takes about **2.1 ns**, compared with about **50.7 ns** in the fresh baseline run.

The first inlined-base experiment caused a 3.6% paired regression for 32 elements/one round. Outlining both branches removes the dispatcher's register save area and reduces that regression to **2.5%**, rather than eliminating it. All other final paired broad medians change by less than 1.9%, excluding the intended zero-round gains. The original experiment and its regression remain in the raw evidence.

The three complete runs meet the 10% threshold against the faster C++/Rust median in **32/32, 31/32 and 32/32** cases, with lower medians in **27/32, 28/32 and 27/32**. The predeclared third run gives nine independent processes per language/case in total. Aggregating all nine process medians, with none discarded, meets the 10% threshold in **32/32** and beats both references in **27/32**. The five remaining slower cases range from 0.2% to 4.9% behind the faster reference: the goal of beating both everywhere is still open.

GCD/16 in the second run takes 188.8 ns against Rust's 154.8 ns: **21.9% slower**. Its generated LLVM and entire 42,312-byte machine-code text segment are byte-identical to baseline, and paired time improves 0.6%; its second-run process medians nevertheless vary from 161.6 to 206.6 ns. Across all nine processes its median is 164.0 ns, **4.6% behind** the faster reference. The missed individual run is retained rather than overwritten. All eight legacy array/closure controls meet the 10% threshold; seven have lower medians than both references. These are measured cases on this host, not a universal language ranking or physical macOS 26 qualification.

## Complete comparison

| Program | Rounds | Baseline K ns | Aggregate K ns | Aggregate C++ ns | Aggregate Rust ns | Aggregate vs faster reference | Second run vs reference | Paired time reduction |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| affine | 0 | 0.896 | 0.896 | 1.167 | 1.179 | -23.2% | -24.1% | -0.0% |
| affine | 1 | 0.903 | 0.897 | 0.897 | 0.895 | +0.2% | -0.1% | -0.3% |
| affine | 16 | 4.302 | 4.286 | 7.745 | 7.736 | -44.6% | -44.4% | +0.8% |
| affine | 256 | 386.139 | 370.947 | 374.132 | 376.640 | -0.9% | -1.2% | +0.2% |
| polynomial | 0 | 0.941 | 0.946 | 1.189 | 1.184 | -20.1% | -20.1% | -0.3% |
| polynomial | 1 | 0.938 | 0.937 | 0.893 | 0.902 | +4.9% | +6.3% | +0.1% |
| polynomial | 16 | 4.740 | 4.751 | 7.604 | 7.812 | -37.5% | -39.3% | -0.1% |
| polynomial | 256 | 380.221 | 374.332 | 372.251 | 371.763 | +0.7% | +1.0% | -0.2% |
| euclid | 0 | 1.171 | 1.192 | 1.184 | 1.195 | +0.6% | +0.6% | +0.4% |
| euclid | 1 | 2.967 | 2.951 | 3.286 | 4.059 | -10.2% | -10.1% | +0.3% |
| euclid | 16 | 160.811 | 163.979 | 156.743 | 158.878 | +4.6% | +22.0% | +0.6% |
| euclid | 256 | 3277.303 | 3167.325 | 3200.162 | 3243.481 | -1.0% | -2.8% | +0.4% |
| array8 | 0 | 6.201 | 6.242 | 22.886 | 21.734 | -71.3% | -72.1% | -0.1% |
| array8 | 1 | 10.739 | 10.894 | 51.802 | 49.875 | -78.2% | -78.2% | +0.0% |
| array8 | 16 | 123.386 | 123.257 | 470.734 | 444.360 | -72.3% | -72.1% | +0.8% |
| array8 | 256 | 1908.846 | 1904.426 | 7700.358 | 7303.883 | -73.9% | -74.1% | +0.9% |
| array16 | 0 | 12.293 | 12.525 | 25.636 | 24.100 | -48.0% | -48.6% | -0.3% |
| array16 | 1 | 27.265 | 27.038 | 62.619 | 61.282 | -55.9% | -56.2% | +0.1% |
| array16 | 16 | 260.232 | 256.434 | 574.532 | 572.618 | -55.2% | -55.3% | -0.0% |
| array16 | 256 | 3869.415 | 3839.708 | 9516.085 | 9095.703 | -57.8% | -57.7% | -0.5% |
| array32 | 0 | 10.113 | 2.091 | 28.791 | 25.672 | -91.9% | -91.8% | +79.0% |
| array32 | 1 | 34.430 | 34.807 | 73.099 | 71.016 | -51.0% | -46.1% | -2.5% |
| array32 | 16 | 484.377 | 476.313 | 733.059 | 657.505 | -27.6% | -27.6% | -0.3% |
| array32 | 256 | 7393.505 | 7405.521 | 11853.228 | 11274.256 | -34.3% | -33.5% | +0.0% |
| array64 | 0 | 21.206 | 2.414 | 39.478 | 31.200 | -92.3% | -92.2% | +88.5% |
| array64 | 1 | 80.046 | 78.678 | 108.131 | 97.888 | -19.6% | -20.6% | +1.8% |
| array64 | 16 | 941.072 | 930.687 | 1130.561 | 1085.995 | -14.3% | -14.2% | +0.0% |
| array64 | 256 | 14719.577 | 14599.401 | 18089.756 | 17422.482 | -16.2% | -16.2% | -0.3% |
| array128 | 0 | 50.655 | 2.091 | 62.095 | 53.466 | -96.1% | -96.2% | +95.9% |
| array128 | 1 | 155.098 | 153.689 | 186.135 | 160.165 | -4.0% | -4.0% | +0.3% |
| array128 | 16 | 1796.174 | 1801.351 | 2036.475 | 1903.486 | -5.4% | -6.0% | -0.0% |
| array128 | 256 | 27765.225 | 27836.824 | 34647.853 | 31978.902 | -13.0% | -13.8% | +0.8% |

Aggregate K/C++/Rust columns use the median of all nine independent process medians from three complete runs. The second-run column retains its individual misses. Negative reference differences mean less time; positive paired reductions mean improvement. Each complete run uses three independent processes per language/case, two warmups and seven measured adaptive batches targeting 10 ms. The paired study uses nine adjacent ABBA/BAAB blocks with equal iteration counts per group. Checksums include every timed and warmup return. Every standalone corpus also checks multiple runtime seeds against independent oracles. CPU affinity, frequency and desktop activity were not isolated.

## Proof and language boundaries

The transformation operates on a speculative copy of an already-proven ordinary private-buffer entry. Standard LLVM unrolling and scalar replacement either eliminate all loops and allocations or the candidate is discarded. Forced expansion requires non-nested constant trip counts of at most 128, at most 32 instructions per loop and at most 4,096 expanded instructions overall. Admission also requires at most 64 final instructions, at least a threefold size reduction and the existing 16 KiB frame check. These are optimization budgets, not language size, rank or recursion limits.

The allocation-policy query and terminal generated heap replay remain observable. Failed allocations, out-of-domain inputs and intermediate overflow retain checked evaluation, settlement and first-fault reporting. The two outlined workers have identical scalar/fault signatures and mandatory tail transfers. Public arguments acquire no new range assumptions. The final dispatcher contains only a condition and tail branches, with no stack save area. Where the source returns a float, this guarded benchmark result uses one hardware conversion; the integer sum is below 2^53 and exactly representable. Integer-returning programs need no such conversion. Floating order and flags do not change.

Structural tests verify fixed-width folding, rejected budget/effect candidates, idempotence and both mandatory tail branches. Source reduction tests now compare successful direct exports with checked and recoverable results as well as failure paths. They exercise widths 2, 8, 32 and 128; int64, float64 and Bool results; six allocation-failure settings; and 225 input pairs including negative values, high bits and intermediate overflow. Four local modes produce **64,800** comparisons; hosted Windows runs three modes and **48,600** comparisons. This extends the existing arithmetic, alias, cleanup and cancellation matrices.

## Evidence

[Nine-process aggregate](wide-reduction-outlined-aggregate-2026-10-28.json), [raw-observation aggregate audit](wide-reduction-outlined-aggregate-audit-2026-10-28.json), [third complete run](wide-reduction-outlined-confirmation-2026-10-28.json), [third-run independent audit](wide-reduction-outlined-confirmation-audit-2026-10-28.json).

[Final dispatch disassembly and unchanged GCD machine-code proof](wide-reduction-machine-qualification-2026-10-28.json).

[Fresh baseline](wide-reduction-baseline-2026-10-28.json), [final run](wide-reduction-outlined-2026-10-28.json), [repeat](wide-reduction-outlined-repeat-2026-10-28.json), [paired study](wide-reduction-outlined-paired-2026-10-28.json), [independent oracle/statistics/artifact audit](wide-reduction-outlined-audit-2026-10-28.json).

[Legacy controls](wide-reduction-outlined-controls-2026-10-28.json), [paired controls](wide-reduction-outlined-controls-paired-2026-10-28.json), [closed-form oracle audit](wide-reduction-outlined-controls-audit-2026-10-28.json), [paired audit](wide-reduction-outlined-controls-paired-audit-2026-10-28.json).

[Initial inlined experiment](wide-reduction-current-2026-10-28.json), [initial repeat](wide-reduction-repeat-2026-10-28.json), [initial paired study](wide-reduction-paired-2026-10-28.json), [initial audit](wide-reduction-audit-2026-10-28.json). Files without `outlined` identify this superseded prototype, not the shipping compiler. Both compiler variants remain distinct in the frozen artifact hashes.

Final qualification passed **432 selected CPU regressions** (six focused plus 426 broader tests) and **50 selected Metal-suite tests**, with zero failures, disabled tests or skips. The Metal execution tests ran against this Mac's physical GPU. The final explicit two-tail-transfer structural assertion was rebuilt and passed separately after its addition; that output and the final Win64 code-generation rerun are appended to the focused log. All 1,605 frozen source/artifact hashes match.

Actual hosted Windows debug/optimized/ASan-UBSan execution passed the expanded reduction matrix, scalar/typed-buffer ABI checks and terminal cleanup. Linux and Windows reproduced their standalone packages and qualified downloaded packages. macOS standalone qualification, signing, notarization, stapling and assessment passed for the final source. Publication was skipped for the branch run.

An initial code-inspection command rewrote two local GCD benchmark executables. Frozen-hash validation rejected reuse before any further timing. Relinking the recorded original commands restored both original SHA-256 hashes exactly; subsequent extraction used copies, and all earlier artifacts passed independent re-audit. The confirmation run used the restored originals.

[Qualification manifest and frozen hashes](wide-reduction-qualification-2026-10-28.json), [numerical records](wide-reduction-numerical-qualification-2026-10-28.json), [Windows execution records](wide-reduction-windows-qualification-2026-10-28.json), [focused tests](wide-reduction-focused-qualification-2026-10-28.log), [focused JUnit](wide-reduction-focused-qualification-2026-10-28.xml), [CPU log](wide-reduction-cpu-qualification-2026-10-28.log), [CPU JUnit](wide-reduction-cpu-qualification-2026-10-28.xml), [Metal log](wide-reduction-metal-qualification-2026-10-28.log), [Metal JUnit](wide-reduction-metal-qualification-2026-10-28.xml), [full build](wide-reduction-build-qualification-2026-10-28.log).

[Windows qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40607200984), [Linux qualification](https://github.com/kalandra-lang/kalandra/actions/runs/40607201010), [macOS qualification and notarization](https://github.com/kalandra-lang/kalandra/actions/runs/40607200860).
