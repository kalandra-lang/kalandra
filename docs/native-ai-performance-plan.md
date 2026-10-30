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

## Attention contraction parallelism and guarded residual fusion

Completed the next two experiments against frozen `2fb73f53`. The existing
contraction-parallel kernel now covers small output rectangles with a long inner
dimension. All 12 newly selected general matrix cases improve 17.4–56.0% in
five-block confirmation, with every block positive. The initial policy-only
whole-transformer screen remains mixed and is retained.

The compiler can reorder a map's parameter ABI while preserving expression
operand order. Runtime fusion crosses an optional EqualShape only when it
already passes from predicted output metadata, retaining staged fault order
otherwise. Binary residual epilogues reduce 14 dispatches to 12 and the largest
block from seven submissions to six. The existing logical/work cancellation
budget and oversized-fused-dispatch rule remain; physical checkpoints move
within that contract. See the [design](design/gpu-guarded-matrix-epilogues.md).

The full-block five-block comparison improves 19/27 medians. The largest case
uses 11.4% less resident time (all five blocks), 9.5% less with activation upload
and 4.8% less with all uploads. A separate replication gives 12.8%, 7.6% and
5.5%. Fresh resident times are CPU 10.72 ms, Kalandra Metal 1.71 ms and MPSGraph
0.52 ms: 6.27× our CPU speed, still 3.26× MPSGraph time. A fusion-only control
with identical new shaders/header and the same attention policy supports the
large-case gain. Small-workload parity is not established: losses of 3.2%/4.4%
in the first study shrink to 1.0%/2.0% in the replication. A single-row staging
rule is mixed and does not solve them consistently, so it is rejected.

Four-mode qualification passes 1,745 APFloat cases / 1,769,284 checks per mode,
expanded binary IEEE and operand-order fixtures, every 4/6 failure boundary,
allocation failures, aliases, concurrency, fallback and generated source entry.
All-ten-type source integration passes in all four modes. A shader-validation
notice filter was corrected without weakening numeric or exit checks. Old
compiled program metadata is compatible with the new runtime, CPU LLVM and the
task archive are unchanged, and the canonical shader ABI swap is checked exactly.
Pipeline setup is about 0.28 ms higher in separate startup observations; warm
acceptance excludes setup. Every outcome and the remaining transfer/latency gap
are in the [report](../benchmarks/results/gpu-attention-fusion-2026-10-29.md).

## Owned shared-upload snapshots

Completed the unified-memory upload experiment against frozen `dec5a66d`.
CPU inputs now copy into owned shared Metal storage, without a staging/private
blit or GPU submission. Copies yield every 1 MiB, retain the source allocation
lease, check cancellation and publish only complete snapshots. Non-unified
devices retain the acknowledged private-buffer fallback. Arithmetic, shaders,
compiler, TaskCore and compute cancellation boundaries are unchanged. The
[design](design/gpu-owned-upload-snapshots.md) records the lifecycle contract.

Five paired blocks improve all-upload medians **26.4–56.0%**, with all five
blocks positive in every case. Activation-upload medians improve **10.5–17.6%**.
An independent repetition confirms **25.3–51.6%** all-upload reductions. The
all-upload calls fall from 11–13 GPU submissions to 4–6. Resident results remain
mixed: primary losses reach 2.7%; the repeated small cases change signs and
include a 2.9% loss at 1×256. A same-binary private/shared control preserves
these small tradeoffs rather than attributing every change to the new code.

Both MPSGraph and Kalandra use fresh owned shared snapshots for the new matched
comparison, including allocation and CPU copying inside timed uploads. The
largest resident block takes 1.66 ms on Kalandra Metal, 10.62 ms on Kalandra CPU
and 0.51 ms on MPSGraph: 6.42× our CPU speed, still 3.25× MPSGraph time. With all
inputs uploaded per call, Metal/MPSGraph take 3.14/1.91 ms. Small GPU workloads
still favor CPU, and this is one transformer block, not full-model inference.

Four-mode upload, ten-format/rank-0–8 storage, compute, numeric and prepared-graph
qualification passes, including partial-copy cancellation/deadlines, exact byte
and admission budgets, allocation failures, source leases, independent snapshots,
concurrency and teardown. APFloat again passes 1,745 cases / 1,769,284 checks per
mode. A child/group deadline assertion was corrected without changing runtime
behavior. The acknowledged and unbounded-copy screens remain benchmark-only.
A five-block write-combined-cache experiment is rejected: 1×512/64×512 all-upload
cases lose 20.8%/11.8%, with every block negative. Production uses default caching.

See the [audited report, chart and all raw evidence](../benchmarks/results/gpu-owned-uploads-2026-10-29.md).
Next: resident scheduling/dispatch latency, including whether work-bounded
chunks can replace the fixed four-stage cap while retaining a clear cancellation
and resource contract. Any change to that contract needs explicit qualification;
unbounded graph submission is not the proposed shortcut.

## Work-bounded resident progression

Completed against frozen `c29e4b14`. Compiler-costed hardware float32 plans now
fill the existing 64 Mi estimated-work budget without also stopping at four
logical stages. The four local parameter slots rotate into retained ownership.
Unknown or missing proofs, software arithmetic, tight budgets and long plans
retain the compatibility path. Cheap stages can share a checkpoint: this changes
cancellation granularity, while preserving cancellation/fault precedence and
acknowledgement-before-cleanup. It is not a wall-clock deadline guarantee.
See the [contract](design/gpu-work-bounded-progression.md).

All 27 primary medians improve: **10.5–55.8% less resident time**, **10.7–54.9%
less activation-upload time**, and **3.6–51.1% less all-upload time**. One of 135
primary blocks regresses and remains included. Same-binary policy control has
positive medians and blocks throughout; the independent repeat confirms
**53.5–54.9%** resident reductions for the one-token cases, **25.2%** for 16×512,
and **10.8%** for 64×512. Submission counts fall from 4 to 1 for small cases,
5 to 3 for 16×512/64×256, and 6 to 5 for 64×512, retaining 12 dispatches.

Fresh largest resident times are GPU **1.53 ms**, CPU **10.90 ms**, MPSGraph
**0.52 ms**: **7.10× our CPU speed, still 2.93× MPSGraph time**. All-upload times
are GPU/MPSGraph **3.13/1.97 ms**. Tiny workloads still favor CPU. MPSGraph is a
benchmark reference only; no production numerical dependency is added.

Optimized, debug, ASan/UBSan and TSan qualification covers every held
cancellation/deadline/device-failure boundary, allocation failures, concurrency,
changing captures, aliases spanning owner windows, rank-1/3/8 chains with
5/9/65/513 stages, expensive/saturating costs and partially missing proofs.
Broader arithmetic, pipeline, ReLU, graph-batching, tensor-transform, storage,
upload and numeric checks pass. A stale register-selection assertion was fixed
and numerical cases were added on both sides of that threshold. Compiler tests
check growing aggregate expression costs and absence of software proofs.

All 23 shader text artifacts and CPU LLVM remain unchanged; measurements reuse
the original Metal binaries. Every Kalandra result cell matches the baseline
across 13 regular/odd shapes and all residency modes. Independent CPU/MPSGraph
numerical bounds pass. Diagnostic profiling separately confirms dispatch counts;
acceptance timing leaves those counters disabled.

An initial full run under Android Studio load and a subsequently interrupted
attempt are retained as diagnostics, not acceptance. All four acceptance studies
were restarted after the user's explicit idle confirmation, with process-load
samples retained. No tests, builds, GPU profiling or plotting overlap them.

A longer paired profile places **0.51 ms inside GPU command intervals out of
1.56 ms wall time** for the largest candidate, with 113 allocation checkpoints.
This leaves substantial setup, submission, acknowledgement and queue-gap costs,
but does not isolate one cause. Next: measure command-buffer creation, buffer and
argument allocation, and inter-command gaps separately before choosing a bounded
reusable-storage or submission-service design. Changing that architecture must
retain the same cancellation, ownership and concurrency qualification.

See the [audited report, chart, all outcomes and reproduction](../benchmarks/results/gpu-work-bounded-progression-2026-10-29.md).

## Invocation-private parameter arena and host-cost profile

Completed against frozen `fb85c832`. On the large baseline, diagnostic median
intervals are about 0.49 ms from submission to GPU start and 0.38 ms from GPU end
to callback entry, compared with 0.08 ms in buffer allocation. Three completion
worker prototypes (global default, global interactive and serial initiated)
failed to provide a reliable improvement and remain benchmark-only. The earlier
shared-event/indirect-grid experiment was also rejected; do not rediscover it
without a materially different hypothesis.

The accepted change packs parameters into one fresh invocation-owned Metal
allocation with distinct aligned writable slices and fault words. It reduces
16 parameter buffers to one and allocation checkpoints from 109 to 95 (small)
and 113 to 99 (large). Budget padding retains the previous 64 KiB headroom; if
it cannot fit, the same command policy uses separate buffers. Cancellation
checkpoints, shader code, fault precedence and acknowledgement ownership are
unchanged. No storage persists across invocations.

Primary resident medians improve **3.0–9.6%**. **26/27** primary shape/mode
medians and **119/135** paired blocks improve. The 16×512 all-upload median is
**0.19% slower**, retained in the report; its same-binary control and independent
repeat improve **1.25%/1.92%**. All control and repeat case medians improve, but
resident replication ranges **0.49–10.73%** and the small gains are noisy. Fresh
64×512 resident GPU/CPU/MPSGraph medians are **1.57/11.15/0.54 ms**: **7.09× our
CPU speed**, still **2.92× MPSGraph time**. Do not compare absolute times between
separate studies to attribute this change's gains.

All 26 broader qualification groups plus four prepared modes pass, including
ASan/UBSan and TSan, every held failure boundary, concurrent captures, exact
budget-boundary fallback, allocation failure and cleanup. All Kalandra cells
match across 13 regular/odd shapes and all residency modes; independent
numerical bounds pass. Every acceptance sample retains the previous submission
count. Tests/builds/profiling do not overlap acceptance timing.

See the [audited report and reproduction](../benchmarks/results/gpu-parameter-arena-2026-10-29.md)
and [storage contract](design/gpu-parameter-arena.md).

## Autonomous continuation window — 29 October 2026

The user authorized continuous work until approximately **10:54 UTC / 11:54
Europe/Zurich**. A thread heartbeat (`kalandra-overnight-optimization`) resumes
this chat every 15 minutes during that window; a timed caffeinate process keeps
the Mac awake while permitting display sleep. Continue immediately between
steps while active. Resume existing jobs rather than duplicating benchmarks.

Sequence, adapting only to measured evidence:

1. Commit/push the qualified parameter arena to `native`, update PR #260 to
   `develop`, preserving Closes #13, #233 and #135; inspect hosted checks.
2. Profile and screen compute-pass/encoder overhead within the current work
   chunks. Test encoder reuse with explicit dependency barriers and complete
   resource/fault handling. Keep command/cancellation boundaries fixed.
3. Investigate avoidable data movement and kernel boundaries, particularly
   transposed RHS consumption in attention and proven pointwise/row operations.
   Preserve exact arithmetic trees, aliases and first-fault behavior.
4. If handoff latency still dominates, test a materially different submission
   or checkpoint architecture with an explicit semantic contract. Existing
   event/indirect-grid and completion-worker variants have negative evidence.
   Do not quietly trade cancellation guarantees for benchmark wins.
5. After each candidate, independently verify results, use frozen interleaved
   controls and replication, reject regressions, document architectural
   decisions, and push only qualified improvements. Keep production numerical
   code independent of vendor libraries. Fix CI regressions as they arise.
6. Stop starting experiments near the end of the window; leave a coherent tested
   state, disable the heartbeat and summarize commits, measured gains, rejected
   experiments and remaining gaps. No merge or tagged release is authorized.

### Encoder reuse screen — rejected

Both serial encoder prototypes passed 72 exact output comparisons with Metal validation, but neither produced a reliable cross-case gain. Keep the existing per-dispatch encoders. [Full negative evidence](../benchmarks/results/gpu-encoder-reuse-screen-2026-10-29.md) is retained. Continue with transpose/matmul dataflow fusion, preserving the fixed arithmetic tree and shape guards.

### Qualified: contiguous transpose views

General direct/transposed matrix profiles regressed the larger workloads; retain
those experiments without changing production kernels. The narrower singleton
and empty transpose can share contiguous immutable backing. Backing alias safety, all ten types, four runtime modes and 26 broader test
groups pass. Frozen/control/repeated timings plus 78 separate streaming output
comparisons are retained in the [acceptance report](../benchmarks/results/gpu-transpose-view-2026-10-29.md).
One-row resident gains are modest (primary 0.37–3.07%, replication 0.98–4.01%);
confirmation includes a nearly flat 1×256 result. Inactive regression controls
move both ways and remain published. No blanket speedup is claimed. See the
[design and safety contract](design/gpu-contiguous-transpose.md).

After this candidate is settled, investigate invocation-private intermediate
buffer allocation. Any arena must keep escaped outputs independently owned,
preserve physical offsets and live-alias checks, and fit the existing byte
budget. Do not trade smaller allocation overhead for retaining an entire arena
behind a small returned tensor. Submission handoffs remain the larger unresolved
cost; a pipelined design requires an explicit cancellation and failure-order
analysis before any production policy changes.


### Next: overlap safe encoding with the first GPU chunk

Screen an isolated runtime that submits the first complete checkpoint while the
CPU encodes subsequent checkpoints. Keep one command in flight, the same work
limits, and acknowledgement/fault/cancellation checks before each later commit.
The completion lease must survive until both encoding and submitted work end.
Protect shared owner/checkpoint vectors; cover every early return and allocation
failure with a builder-exit guard. This is an architectural experiment, not a
production policy change. Qualify concurrent teardown and cancellation before
acceptance. Intermediate-buffer arenas remain the following independent option.

The overlap candidate now includes an atomic callback handoff, deterministic
negative-control regression, and separate parameter backing for concurrently
encoded commands. The original prototype's callback gap is retained as rejected
evidence; no such version was promoted to production. Four-mode prototype
fixtures and fixed budget boundaries pass. Production qualification passes all four prepared modes and 27 broader groups.
Frozen/control/repeated and longer-batch acceptance are complete. Largest resident
gains are consistently about 4%; smaller controls remain mixed, including roughly
1–3% regressions. No blanket speedup is claimed. The complete
[report](../benchmarks/results/gpu-encoding-overlap-2026-10-29.md) retains all
negative results and background-load samples. See the
[ownership and admission design](design/gpu-encoding-overlap.md).

After acceptance, separate remaining device arithmetic from submission costs.
A benchmark-only single-command or closed synchronous-entry lower bound may help
quantify the architectural gap, but must explicitly disclose any cancellation
policy difference and cannot be presented as the default language. Do not relax
arithmetic or cancellation contracts merely to improve the comparison.

### Measured: closed-entry lower bound

The [diagnostic controls](../benchmarks/results/gpu-command-lower-bound-2026-10-29.md)
keep shaders and all 11/12 dispatches identical. Largest resident wall time is
1.452 ms bounded, 0.847 ms indivisible, 0.840 ms caller-blocking and 0.537 ms
MPSGraph. These are instrumented diagnostics, not production acceptance. All 117
full-output comparisons pass; the controls deliberately lack the ordinary task
cancellation contract. The initial wrong-checkpoint probe was rejected by its
submission-count assertion and is retained.

Implement an opaque closed runner, with no exposed TaskCore or cancellation
handle. Permit synchronous whole-region GPU execution only after a dynamic proof
that this private core has no other runnable/executing/preparing/external work.
Current deadlines are explicit cancellation capabilities, not an internal timer;
future timers must also invalidate the proof. Waiting ancestors are safe. General
TaskCore calls and parallel source work retain the bounded asynchronous path.
Integrate the runner into generated standalone Metal entry points, then qualify
source behavior, ownership, failure injection and concurrent callers before fair
uninstrumented comparisons. Device arithmetic/dispatch cost remains a separate
roughly 0.48 ms versus MPSGraph's 0.25 ms in the large diagnostic.

### Qualified: closed synchronous GPU entry

The private runner and standalone compiler entry are committed as `259b9ce7`;
see the [contract](design/gpu-closed-entry.md). Ordinary task embedding and source
parallelism retain cancellation and independent progress. Only an admitted pure
region in an isolated private core omits command checkpoints. Arithmetic, shaders,
resource admission and fault precedence are unchanged.

All 1,407 tests are qualified across the full run and 62 affected fixture rechecks.
Initial array-cancellation observer, upload-command ordinal and completion-prefix
fixture failures remain in the evidence. All four runner and GPU sanitizer/build
modes pass. The separate acceptance sweep checks 286 complete outputs over 13
regular/odd shapes and upload/fallback policies against frozen bits and independent
CPU/MPSGraph bounds.

The [audited report](../benchmarks/results/gpu-closed-entry-2026-10-29.md) and
[chart](../benchmarks/results/gpu-closed-entry-2026-10-29.png) retain every paired,
policy, ordinary-API, replication, streaming and fresh-reference outcome. Largest
resident elapsed time falls 43.32% primary, 43.38% replication and 42.79% against
the same-binary bounded policy. Fresh medians are CPU 10.690 ms, Metal 0.841 ms,
MPSGraph 0.536 ms: 12.71× CPU speed, still 1.57× MPSGraph time. The primary's
worst small median is −1.60%; policy control −4.02%; ordinary API −1.85%.
These results do not establish a blanket improvement or statistical equivalence.

All measurements ran sequentially after qualification. One initial calibration
attempt stopped when the Codex renderer exceeded the load guard; its load record
is retained and contributes no accepted paired samples. The resumed six-study
series finished before 10:12 UTC with ordinary background apps active. All
benchmark processes have stopped; no duplicate orchestration is needed.

### Measured next target: FFN kernels

The [encoder counter diagnostic](../benchmarks/results/gpu-dispatch-counter-summary-2026-10-29.md)
ran after acceptance. Eight full bit comparisons pass with Metal/shader validation.
Across all five large-shape processes, FFN expansion and contraction account for
42.1–42.5% of the sum of median inclusive encoder intervals. This is a prioritization
signal, not a fraction of wall time: instrumentation increases overhead, intervals
can overlap, and absolute results drift roughly threefold. Pipeline labels are
unavailable; instruction identities are checked against the frozen program.

A [sustained follow-up](../benchmarks/results/gpu-dispatch-sustained-summary-2026-10-29.md)
uses the exact frozen executable, 256 warmup and 384 measured calls per process.
Its largest-shape command interval varies about 1.3% across five process medians;
FFN expansion/contraction account for 43.33–43.47% of summed median intervals.
All 12,800 call/resource checks pass. Two collector assertion mistakes and their
raw outputs remain retained; production and shaders are unchanged. This narrows
the next target but still does not make instrumented intervals acceptance data.

Next test FFN tile/data reuse without changing per-cell multiplication or
accumulation order. Compare whole-block
and upload timings, odd shapes, constrained budgets, source cancellation and all
output bits. Batched Q/K/V dispatch is another independently measurable candidate.
SIMD-group matrix arithmetic or reassociation requires a separate, explicit numeric
contract decision; it must not silently replace strict arithmetic. No new kernel or
vendor dependency is promoted on the basis of these diagnostic intervals.

The production commit `259b9ce7` passed hosted Linux, Windows and macOS
qualification, including signed/notarized standalone distribution. The subsequent
benchmark-only head `8634a0bc` is undergoing its own qualification; do not equate
an earlier green run with completion of that new run. The next concrete
[output-reuse experiment](design/gpu-ffn-output-reuse.md) documents candidate tile
layouts, unchanged arithmetic trees and occupancy risks. Detailed continuation
notes remain in `/private/tmp/kalandra-overnight-status.md`.


### Screened: strict FFN output reuse and pipeline limits

[Ten isolated controls](../benchmarks/results/gpu-output-reuse-2026-10-29.md) preserve
all tested output bits but do not qualify a new production policy. This includes
adjacent/spaced column reuse, row reuse, four-output tiles, explicit 256-thread
pipeline limits, and 32/128-leaf shared chunks. There are 720 passing validation
case executions with independent finite/signed-zero CPU tree checks, GPU bit
controls, odd tails, immutable inputs and output guard bytes. No arithmetic or
runtime code changed. Larger layouts regress despite fewer explicit loads; the
thread-limit control does not establish a spill/occupancy explanation.

The two 32-leaf timing series have external-load spikes and remain contended
diagnostics. After the user confirmed idle workloads, a seven-block confirmation
peaked at 185% sampled external CPU load, versus roughly 600% earlier. It loses
all seven blocks on each of six shapes, by median 7.90–12.31%, with all output
checks passing. Keep the existing 64-leaf chunks. The 128-leaf variant's small wins are
retained alongside its large regressions. Inspect actual register/occupancy
behavior before another layout proposal; batched Q/K/V dispatch remains an
independent option. Prior benchmark head `8634a0bc` now has green Linux, Windows,
website and signed/notarized macOS qualification; publication remains skipped.


### Screened: independent Q/K/V dispatch and whole-model controls

The [isolated study](../benchmarks/results/gpu-qkv-dispatch-2026-10-29.md)
passes 162 numerical/guard cases. One-grid batching and three concurrent dispatches
both improve selected isolated projections. The
[whole-model follow-up](../benchmarks/results/gpu-independent-dispatch-2026-10-29.md)
rejects promotion of the copied concurrent runtime: 117 exact full-output cases
pass, but seven-block frozen and same-binary controls retain regressions and
substantial variability. No production shader/runtime or cancellation contract changed.

GPU command diagnostics localize most of the large penalty beyond CPU encoding.
Separate parameter buffers do not remove it. Pipeline reflection exposes writable
input/parameter declarations; a read-only shader control changes that metadata and
passes 234 additional full-model cases, but does not reliably recover performance.
These diagnostics do not establish occupancy, spilling or a specific hazard cause.
The selected `gpudebug profile run` route requires M3/A17 Pro; capture works on M2,
but this route returned no hardware resource measurements. Other profiling routes
are not ruled out.

Keep the rejected controls and default 64 MiB budget failure evidence. Next gate:
try the one-grid batched projection shader in a copied full-model path, with
separate graph admission, all output owners, failure/cleanup and cancellation
qualification. Do not promote concurrent scheduling or relax numerical order on
these results. Prior head `173f1748` has green Linux, Windows and website checks;
macOS distribution qualification was still running at the last check.


### Measured M2 register/occupancy follow-up

The [M2 resource profile](../benchmarks/results/gpu-m2-resources-2026-10-29.md)
closes the earlier measurement gap: Xcode GUI replay works on this M2 although
`gpudebug profile run` does not. The original FFN contraction reports 100 allocated
registers, no spills and 29.167% maximum theoretical occupancy; the rejected quad
reports 254 registers, 592 spilled bytes and a 12.5% ceiling. Measured occupancy is
24.81% versus 6.79%. Independent Instruments target-only samples give 24.58% for
the baseline and show threadgroup reads as its largest measured limiter. These
are diagnostics, not performance acceptance. Next reduce temporary lifetimes and
threadgroup-load pressure while preserving the strict reduction tree; do not
promote another larger output tile on reduced source-level loads alone.

### Resource-guided strict kernel controls

The [follow-up screen](../benchmarks/results/gpu-resource-guided-2026-10-29.md)
qualifies 216 numerical/guard cases and records actual compiler resources before
seven-block paired timing. Sequential 32-leaf subtrees increase registers from
100 to 108. Transposed padded/swizzled right-hand tiles reduce static shared-load
instructions from 80 to 32/35 but increase registers to 110/111; all remain
spill-free. Measured contraction occupancy falls from 24.70% to about 17%, while
the reported theoretical ceiling remains 29.167%. Reported shared bytes read do
not fall. The exact allocation/scheduling cause is not isolated.

No candidate is promoted. The padded layout improves two small cells by about
8%, but regresses the other four by 23–43%; preserving 8 KiB with a swizzle does
not recover broad performance. All unfavorable samples and background-load
records are retained. Production semantics, cancellation and ownership are
unchanged. Next test compile-time contraction-shape specialization to remove
generic bounds and unused reduction-root state, with direct resource measurements,
a generic fallback and bounded pipeline caching before whole-model acceptance.

### Shape-specialization controls and transformer prototype

The [shape study](../benchmarks/results/gpu-specialization-2026-10-29.md)
completes K-only/full-MKN controls, actual Xcode resources, an identical-kernel
noise control, independent replication, and a bounded copied-runtime experiment
covering fused matrix epilogues. Allocated/high registers fall from 100 to 92/88,
temporaries from 52 to 48/44, without spills. The reported occupancy ceiling
rises from 29.167% to 35.417%, but measured occupancy is 24.85%/23.08% versus
24.70%; do not attribute the gains to achieved occupancy.

All 144 isolated validation cases and 117 complete-transformer cases pass.
Full-shape isolated time reductions repeat at 12–20%. Resident transformer
M16/M64 width512 improves in frozen, same-runtime, ordinary and replication
studies, generally around 6–8%; the final M64 replication is larger (14.77%).
Upload results remain inconsistent: the same-runtime M64 input-upload control
regresses 6.12%, and the unspecialized M1 input-upload replication regresses
10.46%. External CPU medians span 324–413%; no idle-GPU claim is made. All 504
paired comparisons, qualification records, exports and hashes are retained.

No production promotion. Six fixed shapes at module load establish an
experiment, not a language policy. Task-time compilation and cache growth are
absent, but new PSO setup/memory cost and cancellation/fault-injection gates
remain unqualified. Production semantics and cancellation behavior are unchanged.

Next test a general two-dimensional threadgroup layout to remove runtime
group/tile-column division and modulo. Separately test full-tile boundary
predicate removal behind exact alignment guards. These may recover the index
simplification benefit without exact-shape PSO caching. If shape specialization
is still needed, design bounded preparation/cache ownership outside cancellable
execution, then qualify cold setup, generic fallbacks, faults and cancellation.


### Qualified general aligned-grid matrix dispatch

The [2D dispatch experiment](../benchmarks/results/gpu-grid-dispatch-2026-10-29.md)
rejects a blanket bounded-2D replacement: its replicated gains are mostly noise.
The [production aligned-tile change](../benchmarks/results/gpu-grid-production-2026-10-29.md)
instead removes runtime tile-coordinate division/modulo and redundant bounds
predicates only when both output dimensions are multiples of sixteen, within the
existing M2 Max register16 selection. Original 1D ragged and old-bundle paths,
grouped/skinny selection and other devices retain their policies. There is one
fixed additional pipeline per eligible plain/fused entry, prepared at module
load; no shape whitelist, task-time compilation or growing cache.

Actual Xcode resources fall from 100 to 94 allocated registers, without spills.
The theoretical occupancy ceiling increases from 29.167% to 35.417%, but measured
occupancy is 23.59% versus 24.70%; an achieved-occupancy improvement is not the
explanation established by this experiment. Broad isolated reductions reproduce
at 12.82–27.13% across five shapes, including irregular reductions and larger
matrices. Final actual-production resident transformer reductions are 7.75% and
7.08% at batches16/64, width512. Input uploads show 6.95%/5.07%; all-weight gains
of 1.23%/1.84% are near the measured noise floor. The identical-build control spans
-1.48% to +2.27%. Earlier short-batch regressions, including an unchanged fallback
at -24.34%, are retained; longer-batch fallback results lie within about 2.2%.

All 480 numerical executions across the experimental and production studies
pass, plus four targeted CTest groups covering optimized, debug, address/undefined
and thread-sanitized builds, allocation-failure sweeps, held cancellation/deadline/
device failures, guards, ownership and legacy bundles. This is not a rerun of the
entire compiler suite. Arithmetic order, fault precedence, cancellation contract,
command boundaries and task-time resource bounds are unchanged. Warm-cache
process startup adds roughly 0.4–0.5 ms; genuinely cold driver-cache preparation
is still unmeasured. Production remains independent of vendor numerical libraries.

Next re-profile the complete production block to separate remaining host/pass
cost from shader time. The aligned contraction still reports threadgroup reads
as its largest limiter (41.51%, versus 29.70% ALU), with 80 static shared-load
instructions. A controlled lane-sharing/reuse experiment must preserve the exact
reduction tree, avoid the earlier quad's spills and the padded/swizzled layout
regressions, and repeat whole-model acceptance. Do not introduce exact-shape
caching or a relaxed numerical contract to explain away this remaining gap.

### Host attribution and operand-sharing follow-up

The [host/lane/reuse investigation](../benchmarks/results/gpu-host-lane-reuse-2026-10-29.md)
completes the profiling and shader-control step above. The instrumented resident
batch64/width512 block spends about 412 µs of 783 µs in GPU execution; allocation,
encoding and submit/wake measurements overlap and must not be added together.
The five lane-mapping/sharing controls are rejected. Sharing reduces the shared-read
limiter but adds instructions and spills (32–848 bytes); the spill-free mapping
control is also slower in the repeat. All 420 numerical shader executions pass.
A retained Xcode debug session was discovered after the initial timing series;
those results remain retained as potentially contaminated, with explicit validity
metadata and separate repeats after closing all task capture windows.

A bounded, isolated backing-reuse probe passes 144 changing-input full-output
comparisons across all runner policies/upload modes, plus eight untouched-production
and independent forward-error checks. It retains at most 14.1 MiB in the timing
screen. The report separates seven-block disabled/enabled results from independent
untouched-production controls. Resident timings and disabled controls fluctuate;
these are feasibility results, not an accepted production speedup. The independent
all-weight-upload reductions are 54.74%/64.45% for batches16/64 at width512,
with disabled controls near 2.22%/0.32%. The resident batch64 result cannot be
attributed to caching because the disabled control improves similarly; batch1
input regresses 3.25%. Retain these limits when selecting the next target.

Next design completion-safe, bounded backing reuse with live **and idle** storage
charged against the resource limit. Define eviction, initialization, cache statistics,
concurrent invocation ownership, GPU retirement and teardown; preserve allocation
failure injection and cancellation/fault precedence. The diagnostic global cache
assumes one caller and keeps idle bytes outside the current ledger, so it is not
an eligible production implementation. Qualify ownership/budget behavior first,
then repeat frozen whole-model paired/control/replication timings. Production
remains at the qualified aligned-grid implementation; no numerical or cancellation
contract changed in this follow-up.

### Qualified bounded shared-upload cache

The [production cache report](../benchmarks/results/gpu-upload-cache-atomic-2026-10-29.md)
completes the reuse step above. Per-runtime storage is capped at 16 MiB/64 entries,
keyed by device/exact physical length, and charged inside the existing payload
budget. Complete overwrite, last-owner retirement with no admitted work, idle
pressure eviction and closed-cache teardown preserve immutable values and GPU
ownership. Zero capacity disables reuse; explicit trim releases idle storage.
Private outputs, staging, scratch and argument arenas are excluded. Logical
allocation failures, arithmetic, fault precedence and cancellation checkpoints
are unchanged. Retained/cache statistics expose the idle-memory tradeoff.

The first implementation locked every reservation/release and is rejected after
a 7.92% resident64×256 longer-batch regression. Its original short-batch results,
including a 45.63% input regression that did not reproduce, remain in the
[first-candidate report](../benchmarks/results/gpu-upload-cache-2026-10-29.md).
The accepted revision uses an atomic total-reservation ledger, locking only
cache mutation and budget-pressure eviction. This is a structural improvement;
the measurements do not prove the mutex caused every earlier timing swing.

The revised runtime passes 200 runtime/support tests in four build modes,
including exact concurrent budget contention, cache ownership/eviction/trim,
allocation failures and cancellation. Fresh numerical qualification passes 432
full changing-input comparisons plus eight independent high-precision oracle
checks. Six long integration fixtures and two numeric CLI TSan suites passed
before the private accounting revision and were not rerun afterward. This is
not a full compiler-suite rerun.

Frozen pre-cache production versus the revised runtime, with three adjacent
one-second-target paired blocks across all twelve cases, reduces all-weight-upload
time by 63.12%, 59.24% and 52.52% for batches1/16/64 at width512, and 22.91% for
batch64/width256. An initial resident16×512 median slowdown of 8.29% triggered a
separately declared seven-block confirmation: -0.97% versus an identical-build
control of -0.81%, with volatile individual blocks retained. No resident-compute
gain or zero-overhead guarantee is claimed. Maximum observed idle retention is
12,713,984 bytes, total retained payload 27,447,296 bytes, below the 64 MiB default
budget. Timings do not overlap local builds/tests/profiling; background load and
all unfavorable results are archived.

Next: profile the remaining resident path with the accepted runtime and correlate
allocation/encoding/submit-wake costs with GPU time. Select a bounded private-output
reuse or command/submission experiment only from that evidence; uploaded-buffer
retirement rules do not automatically make GPU-written output reuse safe. Keep
the existing checkpoint/fault/cancellation contract and the spill-free aligned
kernel as controls. Production remains independent of vendor numerical libraries.


### Resident host attribution and private-output reuse screen

The [audited follow-up](../benchmarks/results/gpu-private-output-2026-10-29.md)
profiles the accepted runtime: resident calls allocate 15–16 Metal buffers,
spending 26–57 µs in allocation and 13–21 µs in destruction scopes. The latter
also include cheap parameter-slice wrappers. Total-budget CAS instrumentation
is about 0.2 µs; it is not the measured bottleneck. Batch64/width512 spends about
414 µs of 764 µs in GPU execution. Scopes overlap and are not additive.

A copied runtime tests exact-size/device/private-kind reuse only for full
Float32 outputs of validated pure prepared progression. It preserves last-owner,
zero-admission retirement and the existing total reservation ledger, sharing a
16 MiB/64-entry idle cap with uploads. Flags, scratch, parameters, transforms,
empty outputs and ordinary primitive execution are excluded. Numerical
qualification passes 576 changing-input full-output comparisons and 16
independent oracles, including odd/small shapes. An independent profile check
adds 24 signed-zero-preserving full-output comparisons and eight oracles.

The first short enabled screen was interrupted by a desktop app crash; its
partial logs/load data are retained and excluded from accepted timings. The
preceding identical-build control spans up to 44.6% paired swings. After user
authorization to resume, five longer blocks show a promising 10.86% largest-case
gain, but the broader three-block replication gives a 2.96% median slowdown
with mixed signs. Batch64/width256 improves 15.31%; the all-weight-upload guard
improves only 1.09%. These cohorts do not establish a reliable general gain.

At that stage the private-output candidate was not promoted; production remained `dd940e07`.
That motivated paired intervals within one long-lived runtime, with identical-build
controls and separate GPU/host counters, to isolate allocation/residency from
driver/scheduling variance before expanding cache architecture. Any production
promotion still requires held-ACK, concurrency, alias, failure, budget and
cancellation qualification. No language contract change is accepted here.

### Same-runtime controls and production prepared-output cache

The [same-runtime investigation](../benchmarks/results/gpu-private-inprocess-2026-10-29.md)
keeps runtime/queue/prepared program/inputs alive across ABBA/BAAB intervals.
Small-workload controls remain noisy even after verified termination of sixteen
replay services holding this task's trace files. Cleanup is necessary but does
not establish the cause of that noise. A separate rootless telemetry diagnostic
finds a 0.919 correlation between GPU interval time and inverse reported DVFS
frequency; that association does not prove sole causality or instantaneous clock
values. All failed controls and the explicitly mislaunched 64-row cohort remain.

The unchanged prototype's 512×512 workload with 1,024 calls per slot passes the
original precision goal (AA -0.22%, all blocks within ±2.89%). Enabled private
reuse reduces time 13.01%, independently repeated at 14.18%. These are prototype
results, kept distinct from production acceptance.

The production implementation combines uploads and private prepared outputs in
one 16 MiB / 64-entry idle pool inside the existing payload budget. Only completed,
fully overwritten Float32 outputs of validated pure progression are eligible;
shared/private kind is part of the key. Fresh metadata, last-owner retirement,
zero admissions, pressure eviction, disable/trim and closed-cache teardown
preserve immutable ownership. Logical allocation failures, numerical behavior,
checkpoint boundaries and cancellation/fault precedence are unchanged. Native
C++ Options/Stats layouts grow and require clients to rebuild; generated module
formats do not change. Separate counters expose each retention class.

Qualification passes all 201 selected GPU runtime/support tests, including the
prepared-program fixture in four build modes. New tests cover alias lifetimes,
opposite prior contents, ranks, exact budget pressure, every allocation-failure
point, held acknowledgement, cancellation/device failure, concurrency, policy
changes, entry caps and teardown. Fresh numerical qualification passes 1,944
changing-input full-output comparisons against frozen accepted production and
18 independent high-precision oracle checks across nine shapes, all runner and
upload modes, both switch orders and three cache caps. This is not a full
compiler/CLI suite rerun.


The [first production policy](../benchmarks/results/gpu-prepared-output-initial-2026-10-29.md)
reproduces the resident gain (13.48% / 13.93%, with a passing large-case control)
but is rejected after all-weight-upload regressions of 2.09% / 3.29%. Every block
in both upload cohorts slows down. Class counters expose one new upload miss per
call: private retention displaces a useful shared upload. This is a measured
cache-policy problem, not a numerical failure. Small controls remain noisy.

The revised policy protects existing uploads at private admission, preferring
private eviction under combined/live-budget pressure. Private entries that cannot
fit beside cached uploads are released; the existing shared subquota still evicts
shared entries when needed. No quota increase is used to hide the regression.
Deterministic 128/256-byte pool tests cover both directions of pressure. Fresh
qualification and acceptance must use this policy's own frozen binaries; the
rejected policy's measurements remain separate.


The upload-protecting revision passes all 201 runtime/support tests again and
fresh 1,944-output / 18-oracle qualification. Its same-runtime resident512×512
control passes (-0.84%, all blocks within ±2.89%); enabled/repeat reductions are
13.08% / 14.00%. Uninstrumented frozen production comparisons show 11.97% / 12.48%.
All larger-case controls pass in that protocol. Small controls remain noisy.

The uploaded-weight case keeps seven shared hits / zero misses, but no private
hits. Frozen comparisons retain 4.08% / 2.68% slowdowns; its cache-disabled
comparison is approximately flat (+0.12%). Distinct cohorts must not be subtracted
as a causal estimate, and GPU-interval variation remains material. This motivates
a conservative empty-pool fast path rather than raising memory quotas or changing
numerical semantics. New qualification and fresh all-case frozen controls are
required for that source. The preceding [report](../benchmarks/results/gpu-prepared-output-priority-2026-10-29.md)
and reconstruction patch retain its complete evidence.


### Qualified completed-buffer reuse and conservative fast misses

The [final report](../benchmarks/results/gpu-prepared-output-fastmiss-2026-10-29.md)
qualifies the production implementation. Empty private pools use the ordinary
atomic budget reservation without locking/scanning. A positive upload-pressure
snapshot may conservatively release private backing; admission still rechecks
under the mutex. Lost optional hits under races cannot reuse pending ownership
or overspend the existing budget. No shader, arithmetic, module format,
checkpoint, cancellation/fault-precedence or immutable-value change.

The final source passes all 201 selected GPU runtime/support tests again
(985.27 seconds), including prepared programs in all four build modes. Fresh
qualification passes every one of 1,944 changing-input full-output comparisons
and 18 independent high-precision oracles. Dispatches are checked in a separate
instrumented counter diagnostic; uninstrumented runs correctly report zero
profiling dispatch counters. Positive submission and logical allocation-attempt
counts are unchanged in each matched workload. This is not a full compiler/CLI
suite rerun.

Uninstrumented resident512×512 elapsed time falls 11.44%, independently repeated
at 11.32%, versus frozen accepted production. Every block improves 10.81–12.48%.
Its identical-build control is -0.08%, all blocks within ±0.78%. The upload-heavy
guard has seven shared hits / zero misses and +3.06% / +6.04% median reductions,
but wide blocks and a +5.07% cache-disabled cohort prevent attributing that change
solely to private reuse or the fast path. Smaller controls fail, so no reliable
small-call total-time improvement is claimed. Earlier negative policies and all
failed controls remain published with source reconstruction patches.

Maximum observed idle retention is 15,728,640 bytes; retained payload 46,141,440
bytes, inside the unchanged 64 MiB default. The combined idle cap remains
16 MiB / 64 entries. Native C++ Options/Stats layouts grew and clients must
rebuild. Production has no Accelerate/MPSGraph dependency.

Next profile final-runtime GPU command/kernel time and dataflow. Keep the
qualified source, strict numerical contract and cancellation behavior as controls;
choose further kernel or submission architecture from measurements rather than
assuming remaining time is host allocation.

### Measured compute gap and SIMD-group architecture boundary — 30 October

The [fresh matched comparison](../benchmarks/results/gpu-kernel-library-gap-2026-10-30.md)
puts current resident512×512 runtime at 2.808× MPSGraph time, independently
repeated at 2.798×. Its identical-build control passes; smaller controls fail.
Twenty-four full-output checks satisfy propagated independent numerical bounds;
the separately instrumented runtime remains bit-identical to production.
Separate encoder intervals identify FFN expansion and contraction as the largest
individual passes (about 616/632 µs at 512 tokens). Counter intervals include
sampling/stalls and must not be summed into a wall-time decomposition.

Two strict wider-output tile experiments each pass 84 full-output checks but
lose performance and are rejected. The first has a failed external load capture,
explicitly retained. Neither failure establishes register pressure as the cause.

The [own-code SIMD-group matrix experiments](../benchmarks/results/gpu-matrix-simd-architecture-2026-10-30.md)
target compute directly. A blocked 32×32 implementation improves the isolated
large FFN expansion/contraction by 2.804×/2.656× across five paired blocks.
It also slows the 64×2048×512 contraction to 0.684× baseline speed. The naive
8×8 version improves those smaller cases, but is much less effective at scale.
All 40 finite/signed-zero qualification cases pass independent full-output
error bounds and input/guard checks. These are exploratory kernel screens without
their own identical-build controls, not accepted whole-model speedups.

The SIMD primitive changes rounding and tested negative zero. No candidate is
selected by production; strict matmul and cancellation semantics are unchanged.
An explicit numerical-contract decision is required before language integration.
The recommended route preserves strict matmul and exposes optimized arithmetic
explicitly. Before promotion: specify exceptional-value behavior, qualify
arbitrary dimensions and precision, retain fused epilogues and cancellation/
resource bounds, then rerun full-model numerical checks and frozen paired,
identical-build and MPSGraph reference measurements. Production remains
independent of Accelerate/MPSGraph. The previous production commit's hosted
distribution qualification, signing and notarization all completed successfully.

### Exact-oracle verification of the matrix primitive

The [expanded correctness investigation](../benchmarks/results/gpu-matrix-exact-model-2026-10-30.md)
checks 29 cases × three implementations × three executions: 350,208 cells,
with Metal API/shader validation, unchanged inputs and guard bytes. Integer
products/sums represent the exact real dot product without floating summation.
Independent integer rounders also model the current separately rounded balanced
tree and sequential fused multiply-add with the hardware underflow policy.
Every tested current result matches the tree model; every tested SIMD result
matches the fused model (NaN payload differences allowed).

This is correctness against two different rules, not numerical equivalence.
Adversarial cancellation can produce exact answer 256, current result 0 and
SIMD result 1; both still pass a conservative absolute-products error bound.
Intermediate overflow can produce NaN in the current tree and infinity under
fused accumulation. Tested subnormals flush in both paths, as permitted by the
existing hardware policy. Ordinary randomized cases meet the finite bound, but
the prototype is not universally more accurate. Keep the existing production
contract while deciding the explicit numerical API. Shape tails, broader
precision characterization, full-model and runtime integration remain separate.
