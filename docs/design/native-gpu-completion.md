# Apple GPU integration completion

N2 completion includes all ten native CPU numeric types (`int8`, `uint8`,
`int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32`, `float64`)
and higher tensor ranks, through the native rank-eight limit. This scope was
explicitly confirmed on 2026-10-22. **N2 is complete for this agreed scope** as
of 2026-10-23. Source/device execution, failure and ownership qualification,
host sanitizers and installed compiler acceptance pass. Storage support or a
shader arithmetic proof alone does not enable a source capability.

- [x] Typed owned storage, transfers and exact CPU/device round trips for every
      numeric type and rank, including empty tensors, offsets and settlement.
- [x] Source construction, host/worker bridges, explicit placement and retained
      device results for those types and ranks.
- [x] Strict elementwise arithmetic and scalar lifting, balanced row-major sums,
      rank-two matrix products and composed resident numerical graphs. Higher
      ranks do not introduce batch-matmul or broadcasting semantics.
- [x] Checked integer overflow, division and fault transport, preserving failure,
      cancellation, deadlines, budgets and cleanup without publishing partial results.
- [x] Binary64 arithmetic with the CPU rounding/NaN/negation contracts, without
      narrowing to binary32 or substituting CPU numerical execution.
- [x] Applicable scalar/tensor control, including selected-path numerical stages
      and shape obligations, scalar task results and explicit conversions.
- [x] Source admission, exact numerical conformance, malformed interfaces,
      allocation/device failure injection, host sanitizers and installed compiler
      qualification. Unsupported programs preserve existing output artifacts.

The numerical shader implementation may use integer bit carriers for floating
values. The initial binary64 proof uses 64-bit limbs for multiplication and
restoring division; it requires neither native GPU double nor 128-bit integer
instructions. Its buffer interface requires MSL 2.3. Existing binary32 shaders
retain their separately qualified MSL contract.

The [typed numerical program implementation](native-gpu-typed-programs.md) now
connects all-type arithmetic, conversion, reductions, matrix graphs, scalar
feedback and selected-path control through owned device stages. The [final qualification record](../../native/metal-proof/evidence/gpu-typed-programs-2026-10-23.json)
records 138 passing checks, 630 source cases per compiler, actual device execution
in four host modes with 1/2/4 workers, failure/lifetime checks and installed
relocation acceptance. All 1,112 audited debug SPIR-V shaders avoid floating
arithmetic, native binary64 types and 128-bit integer types. Additional host
modes use the documented representative conversion/graph/empty-layout cases.
The [capability matrix](native-gpu-capabilities.md) remains the source-admission
contract; CPU language features such as managed closures do not automatically
become shader requirements. CUDA, distributed execution and public C/C++
embedding retain their separate roadmaps.
