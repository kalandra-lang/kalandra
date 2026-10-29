"""Summarize diagnostic inclusive GPU encoder intervals, never acceptance data."""
import argparse
import hashlib
import json
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("input", type=Path)
parser.add_argument("--output", type=Path, required=True)
a = parser.parse_args()
data = json.loads(a.input.read_text())
assert data["acceptance"] is False
for collection in ("source_sha256", "artifact_sha256"):
    for file, digest in data[collection].items():
        assert hashlib.sha256((ROOT / file).read_bytes()).hexdigest() == digest, file
assert len(data["verification"]) == 8 and all(row["bit_identical"] for row in data["verification"])
# Instruction positions in the frozen TransformerProgram.h; fused epilogues
# belong to their producer's encoder, not a separate dispatch.
labels = {9: "Attention RMSNorm", 11: "Q projection", 13: "K projection",
          15: "V projection", 16: "K transpose", 18: "QKᵀ + scale",
          20: "Softmax", 22: "Probabilities × V", 24: "Output projection + residual",
          27: "FFN RMSNorm", 29: "FFN expansion + ReLU", 32: "FFN contraction + residual"}
shapes = sorted({(row["m"], row["d"]) for row in data["records"]})
summary = []
for m, d in shapes:
    records = [row for row in data["records"] if (row["m"], row["d"]) == (m, d)]
    assert len(records) == 5 and len({row["process"] for row in records}) == 5
    expected = sorted(pc for pc in labels if m != 1 or pc != 16)
    pass_times = {pc: [] for pc in expected}
    gpu_times, inclusive_sums, wall_times = [], [], []
    pipeline_names = {pc: set() for pc in expected}
    for row in records:
        assert len(row["samples"]) == 9
        measured = row["samples"][2:]
        for sample in row["samples"]:
            assert [entry["pc"] for entry in sample["passes"]] == expected
            assert sample["nanoseconds_per_tick"] > 0
            assert all(entry["ns"] >= 0 and entry["end"] >= entry["start"] for entry in sample["passes"])
        gpu_times.append(st.median(sample["command_gpu_ns"] for sample in measured))
        inclusive_sums.append(st.median(sum(entry["ns"] for entry in sample["passes"]) for sample in measured))
        wall_times.append(st.median(sample["ns"] for sample in row["observations"] if sample["round"] >= 0))
        for index, pc in enumerate(expected):
            pass_times[pc].append(st.median(sample["passes"][index]["ns"] for sample in measured))
            pipeline_names[pc].update(sample["passes"][index]["pipeline"] for sample in measured)
    ffn_shares = [100 * (pass_times[29][i] + pass_times[32][i]) /
                  sum(values[i] for values in pass_times.values()) for i in range(5)]
    passes = [dict(pc=pc, operation=labels[pc], median_us=st.median(values) / 1000,
                   process_medians_us=[value / 1000 for value in values], pipelines=sorted(pipeline_names[pc]))
              for pc, values in pass_times.items()]
    summary.append(dict(m=m, d=d, command_gpu_us=st.median(gpu_times) / 1000,
                        inclusive_sum_us=st.median(inclusive_sums) / 1000,
                        instrumented_wall_us=st.median(wall_times) / 1000,
                        ffn_inclusive_share_percent=ffn_shares, passes=passes))
lines = ["# Diagnostic GPU encoder intervals", "",
         "Apple M2 Max. A copied runtime samples Metal timestamps at each encoder boundary. Production shaders, arithmetic and dispatch geometry are unchanged; eight full output comparisons pass under Metal validation. Sampling is instrumentation, so these numbers are not performance acceptance and must not replace the unprofiled paired report.", "",
         "Five fresh processes per shape, two warmups and seven measured calls per process. Tables use the median of process medians. Each call contains one block. GPU timestamps are calibrated to CPU nanoseconds using paired device timestamps around the command. Encoder intervals are inclusive and may contain synchronization and scheduling costs; they are not exclusive ALU time or an additive wall-time decomposition.", "",
         "| Tokens × width | Command GPU µs | Sum of inclusive intervals µs | Instrumented wall µs |",
         "|---|---:|---:|---:|"]
for row in summary:
    lines.append(f"| {row['m']} × {row['d']} | {row['command_gpu_us']:.2f} | {row['inclusive_sum_us']:.2f} | {row['instrumented_wall_us']:.2f} |")
for row in summary:
    lines += ["", f"## {row['m']} × {row['d']}", "",
              "| Instruction | Encoder work | Median µs | Process-median range µs |",
              "|---:|---|---:|---:|"]
    for entry in sorted(row["passes"], key=lambda entry: entry["median_us"], reverse=True):
        values = entry["process_medians_us"]
        lines.append(f"| {entry['pc']} | {entry['operation']} | {entry['median_us']:.2f} | {min(values):.2f}–{max(values):.2f} |")
largest = next(row for row in summary if (row["m"], row["d"]) == (64, 512))
shares = largest["ffn_inclusive_share_percent"]
lines += ["", "## Interpretation limits", "",
          f"The two FFN matrix passes account for {min(shares):.1f}–{max(shares):.1f}% of the sum of per-process median encoder intervals at 64 × 512. They remain the leading combined target in every process. This fraction is not their share of total wall time.", "",
          "Absolute intervals vary by roughly threefold between early and later processes. These short instrumented bursts do not control GPU frequency, competing device activity or counter-induced scheduling. The data cannot assign a cause to that drift; establish stable sustained-load profiling before accepting a kernel speedup. Pipeline labels are unavailable (`unnamed`); operation names come from instruction positions in the frozen generated program.", "",
          "Use the largest stable encoder intervals to choose a kernel experiment. These counters alone do not establish memory bandwidth, occupancy, arithmetic throughput or why MPSGraph differs. Fused epilogues are included in their producer. The one-row K transpose is an existing zero-copy view and has no encoder. Keep strict arithmetic order and qualify all output bits before any candidate is considered for production.", "",
          "Timestamp conversion follows [Apple's GPU/CPU time guidance](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time). Raw samples, pipeline-label fields, source hashes and output checks are retained in the input JSON."]
a.output.with_suffix(".md").write_text("\n".join(lines) + "\n")
a.output.with_suffix(".json").write_text(json.dumps(dict(acceptance=False, summary=summary,
    evidence=str(a.input), evidence_sha256=hashlib.sha256(a.input.read_bytes()).hexdigest()), indent=2) + "\n")
print(a.output.with_suffix(".md"))
