# Native AI performance execution plan

Requested 28 October 2026. Work proceeds on `native`, in sequence, without a
separate approval between stages. Existing compiler, runtime, documentation and
benchmark changes are preserved. Vendor numerical APIs are benchmark controls,
not production dependencies.

## Completion rules

- Optimize actual compiler/runtime execution, not recognized benchmark names.
- Keep checked arithmetic, separately rounded floating products, adjacent-pair
  reduction order, immutable inputs, ownership, first-fault behavior and task
  cancellation. Architectural changes must state their contract and evidence.
- Freeze the existing compiler, runtime archives and benchmark executables before
  modifying them. Keep baseline data and unsuccessful experiments.
- Run correctness before performance. Use independent per-cell oracles, odd and
  empty shapes, general strides, IEEE edge cases, allocation failure, cancellation
  and applicable sanitizer gates. Qualify platform-independent compiler changes
  beyond the local architecture where the existing infrastructure permits.
- Separate build/test/profile activity from acceptance timing. Repeat complete
  comparisons, retain variability and regressions, and disclose setup, residency,
  transfers, allocations, output observation and thread counts.
- Prefer faster execution than the references; the existing 10% target remains a
  minimum for the relevant equivalent-source corpus. A numerical-library result
  is not a whole-language ranking, and a feed-forward block is not a full LLM.
- If an experiment fails, reject it or restrict it using a justified general
  condition, record the outcome, and continue to the next stage. Do not weaken a
  language guarantee or silently replace missing execution with a reference.

## 0. Establish a recoverable baseline

Status: complete. Eight baseline artifacts are frozen under ignored
`benchmarks/.build/ai-sequence-2026-10-28/baseline`; the independent CPU runner
`native/benchmarks/ai_cpu.py` preserves historical evidence sources.

1. Preserve the new residual FFN harness, two full runs, profiles, audit and chart.
2. Snapshot the current compiler and optimized task/Metal archives, generated CPU
   IR/executables and source digests into ignored experiment storage.
3. Extend the FFN runner to select a compiler/build directory and a CPU-only
   study, so experimental CPU comparisons need not rebuild or retime GPU controls.
4. Add paired execution using the same driver, data and observations around both
   frozen generated CPU programs. Keep lifecycle and arithmetic policies equal.

Exit: baseline artifacts are addressable and checksummed; repeatable paired
measurement and correctness gates are available before compiler edits.

## 1. Single-row and small-batch CPU matrix execution

Status: selected candidate qualified. Sixteen float32/float64 matrix gates passed
across optimized, debug, ASan/UBSan and TSan modes, including expanded small-row
admission/stride/IEEE cases. The sixteen-column float32 candidate reduces paired
single-row FFN time 80.4%, 86.1% and 89.4% at D=256/1024/2048. Source-level
allocation/integration gates passed in debug, optimized and sanitizer modes.
The final five-block combined comparison and CPU-library refresh are preserved
in `ai-cpu-final-2026-10-28.json` and `ai-ffn-cpu-library-final-2026-10-28.json`.

1. Screen a dedicated hardware-float streaming kernel for one/few rows. Traverse
   contiguous RHS columns instead of repeatedly walking a full matrix row pitch.
2. Preserve the exact reduction tree with power-of-two register subtrees and
   ordered roots; never substitute a sequential/FMA dot product.
3. Test no-pack execution before introducing packing that cannot be amortized by
   a single row. Include narrow columns and odd contraction tails.
4. Integrate a general shape/layout dispatch only after screening; retain the
   existing path for unsupported layouts or losing domains.
5. Qualify dynamic and static shapes, offsets/strides, signed zeros, infinities,
   NaNs, allocation budgets, faults, cancellation and sanitizers.
6. Run paired FFN studies and matrix controls, then refresh CPU numerical-library
   comparisons. State remaining gaps without relying on the weak old CPU baseline.

Exit: qualified improvement or a documented rejected experiment, with complete
small/large shape controls and no unsupported universal performance claim.

## 2. Batched CPU matrix kernels and memory layout

Status: selected candidate qualified. Sixteen float32/float64 matrix gates passed
across optimized, debug, ASan/UBSan and TSan modes. A direct five-block comparison
against stage 1 improves the 4/16-row D=1024 cases 29.1%/30.1%, 32/128-row D=1024
9.3%/7.5%, and the 128-row D=256 case 6.8%. The largest 32×2048 control regresses
0.15%; all raw results are retained. Float64 retains its previous tile. Final
combined-source integration passed. Packing is included in API timing and its
separate K×N allocation is checked; no isolated packing-time percentage is claimed.

1. Separate packing cost from kernel cost for the existing dense wide path.
2. Screen cache panels, loop order, output-column SIMD and register tiles under
   the same separately rounded product/tree contract.
3. Measure small batches, large batches, rectangular/odd matrices, non-unit
   strides and memory-cap pressure. Include packing/allocation in API timing.
4. Consider reusable immutable packed weights only with explicit ownership and
   lifetime accounting; never hide preprocessing from transfer-inclusive results.
5. Integrate the best general selection, run numerical/fault/cancellation gates,
   and recheck stage 1 and the equivalent-source scalar/array corpus.

Exit: accepted or rejected candidate results, with packing-inclusive and kernel
diagnostics distinguished and preserved CPU correctness.

## 3. GPU submission and lifetime planning

Status: qualified and measured. Private single-consumer backing recycling and a final
graph-stage/reduction command path passed the four-mode graph gate with Metal
API/shader validation: 350 APFloat cases and 6,790 output checks per mode,
allocation injection, shared-negative-intermediate and original-input checks,
held cancellation/deadline/device failure, 121 budgets, concurrent graphs and
source execution. A five-block paired FFN study reports resident improvements
of 9.7–21.9% in the four larger batched cases; a longer complete repeat improves
those cases 5.3–23.1%. Small losses (including 2.3% resident and 3.1% all-upload)
are retained. Twelve rebuilt graph/reduction/arithmetic fixtures also pass.
Separate profiling confirms five-to-four submissions for the larger cases,
three-to-two for 128×256, and seven fewer allocation checkpoints in the larger
graphs. Existing work and cancellation allowances are unchanged.

1. Trace production FFN stages, submission boundaries, work allowances and buffer
   liveness separately from throughput measurements.
2. Test device-resident tensor continuations and reusable graph preparation;
   scalar observation belongs at the disclosed API boundary, not every layer.
3. Eliminate avoidable temporary allocations/captures where single-consumer
   ownership proves it safe. Shared values retain immutable behavior.
4. Reduce submissions only within the existing logical-stage/estimated-work
   cancellation contract, or explicitly design and qualify a replacement bound.
5. Reuse backing only after its final device use and acknowledgment. Exercise
   failure during allocation/encoding, cancellation, concurrent graphs, teardown
   and tight budgets with Metal validation and host sanitizers.
6. Repeat resident, activation-upload and all-upload FFN studies. Keep small
   regressions and host variability visible.

Exit: qualified runtime change or documented rejected candidates, with measured
submission/storage effects and unchanged safety/cancellation guarantees.

## 4. Complete transformer-block language coverage

Status: implemented and qualified on the development Mac. The typed-operation
contract is recorded in `docs/design/transformer-operations.md`. Transpose covers
all ten numeric types; CPU row math covers hardware float32/float64 and Metal row
math covers hardware float32. Four CPU/GPU runtime modes, invalid epsilon/empty
axes, bit preservation, strides, allocation failures and cancellation/retirement
passed. The typed float32 matrix bridge passed 1,724 APFloat cases and 1,598,840
cell/aggregate checks per runtime mode. Every full-block timed shape is verified
on CPU/Metal/MPSGraph across all residency modes, and eight source prefixes expose
normalization, Q/K/V, probabilities, attention residual, second normalization and
FFN hidden matrices for independent CPU/GPU checks. All 68 rendered documentation
blocks and their classified executable examples passed verification.

1. Inventory CPU/Metal coverage for normalization, transposition, row softmax,
   attention projections, attention products and residual/feed-forward stages.
2. Define one small pre-normalized transformer block, including head/sequence
   layout, activation, epsilon, masking policy and exact numerical policy.
3. Implement necessary generally useful language primitives on CPU and GPU,
   including semantics/shape checks, compiler/runtime lowering, documentation and
   independent correctness tests. Missing operations must be visible; reference
   code is never presented as Kalandra execution.
4. Validate every intermediate stage against an independent high-precision
   reference, with explicit bounds for transcendental/approximate hardware
   functions. Keep any change of numerical policy opt-in and explicit.
5. Cover singleton/odd dimensions, realistic decode/prefill shapes, non-dyadic
   inputs, numerical extremes, failure, cancellation and resource retirement.

Exit: a runnable complete block on the supported CPU and Metal paths, with
documented primitives and verified outputs; it still does not claim full-model
generation or KV-cache/tokenizer coverage.

## 5. Optimized GPU reference and final comparison

Status: complete on the development Mac. Reference, five-block timing, isolated
instrumented profiling and the audited report/chart are saved in
`benchmarks/results/native-ai-performance-2026-10-28.md`. MPSGraph is benchmark-only.
Resident full-block speedups over Kalandra
CPU reach 1.34× at 16×512 and 2.71× at 64×512. Metal remains approximately
7.5–9.5× behind MPSGraph in the resident comparisons; small blocks favor CPU.
The typed path currently acknowledges 16 submissions. No GPU-library parity or
new C++/Rust-wide ranking is claimed from these results.

Final qualification also includes 624,024 CPU transform cell checks across 56
configurations, 145,332 Metal transform checks across 48 configurations, exact
member/group deadline and device-failure kinds, source-computed resident epsilon,
and an installed/relocated package compiling the new native transform kernels.
All six dynamic ordinary benchmark entry modules retain byte-identical LLVM IR
against the frozen compiler. Hosted Windows/Linux/macOS CPU transform gates are
added; these runs are not represented as completed physical-platform evidence.

1. Use an available optimized GPU API, such as MPS/MPSGraph, strictly as a separate
   reference implementation of the same block. Prefer local SDK documentation;
   use official sources if additional API research is required.
2. Disclose float precision, accumulation, FMA, fused operations, device, batch,
   thread settings, layouts and validation tolerance. Compare equivalent output
   observation and distinguish cold preparation from warm execution.
3. Measure resident weights, activation transfers and all transfers independently;
   include normal allocation, scheduling, synchronization and completion.
4. Run complete repeated studies with independently verified outputs and retain
   all samples, source/artifact hashes, commands and exact comparison boundaries.
5. Profile the remaining gap and state whether it calls for further compiler,
   runtime or kernel architecture work. Do not imply GPU-library parity from a
   CPU-BLAS win.

Exit: final report covering every stage, accepted/rejected changes, qualification,
performance gains and losses, remaining gaps and the next evidence-backed target.

## Execution log

- Baseline: `87a456fd` on `native`; implementation compiler source `a7e82604`.
  Residual FFN studies and current CPU chart are present as uncommitted work.
- Initial evidence: single-row CPU selects the narrow path; 1,666/1,673 sampled
  observations in the 128×1024 case are in the two wide matrix kernels. GPU
  residency and submission boundaries remain material.
- The first unsandboxed CPU screen was saved after read-only hardware metadata
  access was unavailable in the sandbox. Its complete raw record is
  `benchmarks/results/ai-row-register-screen-2026-10-28.json`; the stronger
  eight-column streaming candidate is recorded in `ai-row-stream-screen-2026-10-28.json`.
- Streaming changes traversal and stores aligned register-subtree roots. It does
  not use FMA, reassociate reductions or pack/copy the full weight matrix. A
  bounded root workspace and unit-column-stride/multiple-of-vector-width guards
  retain the original path outside the admitted domain.


## Prepared typed GPU execution follow-up

Completed after stages 0–5: immutable prepared plans, per-call scalar binding,
compiler-emitted non-faulting arithmetic proofs, bounded four-stage/64 Mi-work
chunks and ACK-scoped retirement. All 27 paired case medians improved 18–66%;
CPU LLVM and generated GPU shader source are unchanged. Correctness, first-fault,
cancellation, sanitizer, all-ten-type and relocated distribution gates passed.
See the [detailed report](../benchmarks/results/gpu-prepared-execution-2026-10-28.md).
Projection/pointwise fusion and invocation-local temporary/parameter reuse are
next; MPSGraph remains faster and small workloads still favor CPU.


## Typed epilogues and private reuse follow-up

Completed the next GPU step: adjacent non-faulting float32 matrix epilogues and
private last-use backing reuse with live-alias/original-input protection.
Profiling shows 14 dispatches and 4/7 submissions. The largest resident case
improves 9.9%, but five of 27 case medians are slightly negative and pipeline
setup costs about 0.42 ms more. All outcomes and qualification are in the
[fusion report](../benchmarks/results/gpu-fusion-execution-2026-10-28.md).
Device-side graph progression with cancellation/fault gates is the next
architectural experiment; MPSGraph still has a substantial lead.

## Command progression experiments

Completed the device-progression experiment and its safer completion-driven
alternative. Shared-event/indirect-grid progression is slower in every case in
the isolated replay and remains benchmark-only. The accepted prepared float32
path pre-encodes bounded chunks and commits the next chunk from the previous
completion callback, keeping one external lease and the original cancellation
quantum. Pool reservations prevent finite command-buffer capacity from becoming
an execution limit; 513 maps execute through the 129-chunk streaming fallback.

Against `090d4087`, 24/27 paired medians improve and six improve in all five
blocks. Resident reductions range 1.8–9.3%; the largest resident case improves
2.4%. Three all-upload medians are negative, including a 5.3% regression at
64 × 256. A separate retained-reference control does not provide a general fix.
All numerical, lifecycle, sanitizer, ten-format and relocated-install gates pass.
The compiler and CPU LLVM are unchanged. See the
[complete report](../benchmarks/results/gpu-progression-execution-2026-10-28.md)
and [protocol/lifetime design](design/gpu-command-progression.md).

The GPU result is still 4.1× the MPSGraph time for the largest resident block.
Next targets are per-dispatch kernel profiling and broader fusion, plus a
separate unified-memory transfer experiment with matched vendor controls.
Changing GPU cancellation capabilities requires a compiler/effect proof;
unbounded batching is not the accepted shortcut.

## Dispatch profiling and register selection

Completed stage-level profiling in an isolated instrumented runtime, with
empirically checked CPU/GPU clock units and raw counter samples. Sampling
perturbs execution, so acceptance timings are separate. The trace identified
projection shapes unnecessarily excluded from the existing Register16 kernel.

A general output-area threshold of 8,192 cells replaces 65,536 while preserving
the inner-dimension, alignment and tail guards. All 15 newly admitted general
rectangles improve 11.6–24.1% in five-block testing. All nine affected full-block
case medians improve; resident reductions are 5.0%, 11.8% and 10.4% for 64×256,
16×512 and 64×512. The 18 unchanged-selection controls retain their mixed
positive/negative outcomes. Strict arithmetic, cancellation and ownership are
unchanged; four-mode typed and prepared-graph qualification passes.

The fresh largest resident block takes 1.94 ms on Kalandra Metal, 10.65 ms on
Kalandra CPU and 0.52 ms on MPSGraph: 5.5× faster than our CPU, still 3.7× the
MPSGraph time. MPSGraph's internal submission count and the separate cost of
its arithmetic differences have not been established. See the
[report and reproduction evidence](../benchmarks/results/gpu-register-selection-2026-10-28.md).
Next: contraction-parallel attention scores, broader legal fusion, and a
separate controlled unified-memory transfer experiment. Production keeps its
own numerical kernels.
