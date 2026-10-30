# Benchmarks

Kalandra separates equivalent-source language comparisons from optimized numerical
library comparisons. A fast matrix primitive is not evidence that every language
construct is faster than C++ or Rust.

## Current evidence

- [Equivalent-source CPU comparison](results/wide-input-final-2026-10-28.md): signed
  and wide runtime inputs, correctness checks and workload-specific limits.
  [Compact process summaries](results/cpu-comparison.json) retain both final runs.
- [Pinned MIT Steel comparison](results/mlx-steel-comparison-2026-10-30.md): isolated
  kernel results, numerical witnesses, tile selection and licensing scope.
- [Production Steel qualification](results/steel-production-2026-10-30.md): full-model
  correctness, explicit numerical policies, package checks and provisional timing.
  [Release evidence](results/release-evidence.json) contains the complete current
  study documents, including every failed control and unfavorable diagnostic.

The large resident transformer diagnostic is about 2.5× faster than the preceding
implementation and takes about 1.17× MPSGraph time on the measured M2 Max. These are
**provisional observations**: identical-build controls failed even after an idle
retry. Small cases are mixed. Do not advertise exact speedups or generalize to
other hardware, complete LLM generation or all language programs.

## Run a comparison

Use the [native benchmark guide](../native/benchmarks/README.md) and
[fair comparison suite](../native/benchmarks/fair/README.md). Generated executables,
IR and compiler snapshots live in ignored `.build/`. Raw measurements and logs
are local artifacts; only reviewed release evidence belongs in Git.

Run correctness first. Freeze matching compiler/runtime/toolchain artifacts, run
performance sequentially without builds/tests/profilers, retain warmups and all
unfavorable samples, and report setup, residency, uploads, readback, allocation,
thread counts and numerical policy. Instrumented diagnostics are separate from
acceptance timing. A failed control prevents a precise performance claim.

## Research archive

The earlier experimental reports, raw captures, charts, abandoned kernels and
one-off analysis scripts were removed from the active tree. They remain available
at Git revision `8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7`; use `git show` to inspect
or restore an individual file. Historical links point to that revision.
The cleanup also preserved a local ignored archive, including the uncommitted
Steel studies, before removing anything. Its checksums are in the
[archive record](research-archive.json). It is a local recovery aid, not a release
dependency or a file that CI needs to download.
