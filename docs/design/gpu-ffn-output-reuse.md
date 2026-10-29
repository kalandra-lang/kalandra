# Next FFN experiment: strict per-thread output reuse

Source inspection after the sustained encoder diagnostic, 29 October 2026.
This records the original proposal. The screening outcome is recorded below;
these static load counts are not speedup claims.

At 64 tokens × width 512, expansion is (64×512)×(512×2048) and contraction
is (64×2048)×(2048×512). Both select the existing register16 family, with
fused ReLU/residual respectively: useRegisterMatmul admits both; skinny and
grouped policies do not. Confirm actual selected function names in the next
probe: current MTL pipeline labels are unavailable. The source selection proof
is in MatmulLayout.h, Metal.mm's fusedVariant selection and HardwareShaders.h.

The current register16 shader has 256 threads, one output per thread, a
16×16 output tile and 64-wide K chunks. Each chunk stages 1,024 A floats and
1,024 B floats (8 KiB), with two threadgroup barriers. Each output computes
separately rounded products and the same adjacent-pair binary reduction tree.
Seven explicit binary-counter roots preserve reduction order across K chunks.

Candidate A: 16×32 outputs, 256 threads, two adjacent columns per thread.
Stage 16×64 A and 64×32 B (12 KiB). Reuse each A load across two outputs,
maintaining independent product/reduction trees and two sets of seven roots.
Program-level staged input loads fall from 32 MiB to 24 MiB per FFN matrix
for these full-tile shapes. Shared operand expression loads fall 25% before
hardware coalescing/broadcast. These counts are not measured DRAM transactions.

Candidate B: 32×32 outputs, 256 threads, 2×2 outputs per thread.
Stage 32×64 A and 64×32 B (16 KiB). Reuse both operands across four outputs,
with four independent reduction trees (28 root registers plus temporaries).
Program-level staged input loads fall to 16 MiB per FFN matrix; operand
expression loads halve. More registers and shared storage may reduce occupancy
or spill, so this can regress despite less explicit traffic.

Screen each separately in a copied shader/runtime bundle, keeping scalar,
register16, source and epilogue controls. Preserve multiplication rounding,
left/right tree children, signed-zero/NaN behavior, tail guards and final fused
map order. Validate all output bits, odd/tail and IEEE cases before unprofiled
paired whole-block, upload and standalone-matrix timing. Include small inactive
shapes, 16×512, 64×256, bigger matrices and budget/cancellation fallback.

If promoted, add optional named entry families with matching epilogues and
matching threadgroup geometry; missing older entries must retain the old path.
Validate pipeline resource limits. Do not change cost accounting, checkpoint
bounds, arithmetic policy or generic ABI merely to select a faster tile. Run
four-mode prepared qualification and failure/ownership tests before acceptance.
A SIMD-group matrix intrinsic or FMA/reassociation is a separate numerical
contract decision, not an implicit substitute for this strict experiment.

Occupancy warning: for the 64×512 output contraction, a 32×32 tile leaves only
32 threadgroups (instead of 128). Fewer groups can underutilize the device even
with better reuse. At 16 input rows, a 32-row tile also has half-empty rows.
Screen the 16×32 variant first and retain independent shape-specific controls;
never infer a blanket dispatch-policy win from explicit load counts.

## Screening outcome

The [full experiment report](../../benchmarks/results/gpu-output-reuse-2026-10-29.md)
retains ten controls and 720 passing numerical/guard case executions. Adjacent
columns, spaced columns, two-row reuse and four-output tiles do not qualify for
production. Limiting the pipeline to the 256 threads actually dispatched leaves
the original shader nearly flat and does not rescue those variants. These results
do not directly measure register spilling or occupancy.

The 128-leaf control improves two small cells by about 5% but regresses other
shapes substantially. The 32-leaf control and its repeat encountered CPU-load
spikes; their observed regressions remain diagnostic pending a quiet confirmation.
Production continues to use register16 with 64-leaf chunks. No arithmetic,
cancellation, resource-admission or default dispatch policy changes were made.
