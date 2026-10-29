"""Audit and render closed-entry acceptance after all measurement jobs finish."""
import hashlib
import json
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "benchmarks/results"
files = {name: f"gpu-closed-entry-{name}-2026-10-29.json" for name in
         ("paired", "control", "ordinary", "repeat", "streaming", "matched", "qualification", "load")}
files["interrupted-load"] = "gpu-closed-entry-interrupted-load-2026-10-29.json"
data = {name: json.loads((OUT / file).read_text()) for name, file in files.items()}
sha = lambda file: hashlib.sha256(file.read_bytes()).hexdigest()
for study in ("paired", "control", "ordinary", "repeat", "streaming", "matched"):
    evidence = data[study]
    for collection in ("source_sha256", "artifact_sha256"):
        for file, digest in evidence[collection].items():
            assert sha(ROOT / file) == digest, file
    assert evidence["qualification"]["failed"] == 0
    assert all(row["bit_identical_to_baseline"] for row in evidence["verification"]
               if row["backend"] not in ("cpu", "mps"))
    for row in evidence["observations"]:
        backend, m, d, mode = row["cell"]
        if backend in ("cpu", "mps"):
            continue
        ordinary = 5 if (m, d) == (64, 512) else 3 if (m, d) in ((16, 512), (64, 256)) else 1
        closed = backend == "closed" or (backend == "current" and study != "ordinary")
        commands = 4 if study == "streaming" else 1 if closed else ordinary
        for sample in row["samples"]:
            assert sample["submissions"] == commands * row["iterations"]
            assert sample["gpu_ns"] == sample["encoding_ns"] == sample["dispatches"] == 0
qualification = data["qualification"]
assert qualification["failed"] == 0 and qualification["tests"] >= 1407
assert not qualification.get("skipped")
for file, digest in qualification["sha256"].items():
    assert sha(ROOT / file) == digest, file
assert data["load"]["samples"]


def pairs(evidence):
    result = {}
    for pair in evidence["pairs"]:
        result.setdefault(tuple(pair["group"]), []).append(pair["time_reduction"] * 100)
    return result


paired = pairs(data["paired"])
shapes = sorted({key[:2] for key in paired}, key=lambda shape: (shape[1], shape[0]))
matched = {tuple(row["cell"]): row["median_ns"] / 1000 for row in data["matched"]["summary"]}
largest = {name: matched[name, 64, 512, 0] for name in ("cpu", "closed", "mps")}
reduction = st.median(paired[64, 512, 0])
replication = st.median(pairs(data["repeat"])[64, 512, 0])
control = st.median(pairs(data["control"])[64, 512, 0])
ordinary = pairs(data["ordinary"])
lines = ["# Closed synchronous GPU entry", "",
    "Apple M2 Max, 29 October 2026. Frozen baseline: `6e8e7999`. The new standalone entry privately owns its scheduler; only a proved pure region without independent task/external progress becomes one GPU command. Ordinary `TaskCore` callers retain cancellation checkpoints. See the [entry and ownership design](../../docs/design/gpu-closed-entry.md).", "",
    f"Largest resident elapsed time falls **{reduction:.2f}%** in the primary paired comparison, **{replication:.2f}%** in replication and **{control:.2f}%** against the same-binary closed-but-bounded control.", "",
    f"Fresh resident medians: CPU **{largest['cpu'] / 1000:.3f} ms**, closed Metal **{largest['closed'] / 1000:.3f} ms**, MPSGraph **{largest['mps'] / 1000:.3f} ms**. Metal is **{largest['cpu'] / largest['closed']:.2f}×** our CPU speed and takes **{largest['closed'] / largest['mps']:.2f}×** MPSGraph's time. This does not establish parity with MPSGraph or a blanket GPU advantage for small workloads.", "",
    "Small one-command cases show no consistent gain: the worst primary median is −1.60%, and the worst same-binary policy control is −4.02%. The ordinary cancellable API ranges from −1.85% to +1.38%; this is not an equivalence proof. All negative cells and block counts remain below.", "",
    "## Paired changes and controls", "",
    "Positive percentages mean less elapsed time; negative outcomes are retained. Fresh-process ABBA/BAAB blocks use two warmups and three measured batches per process, with adaptive batches targeting 100 ms. Invocation, selected owned uploads and cleanup are timed. Cold setup and readback are excluded. Counts of positive blocks expose consistency rather than hiding outliers.", "",
    "Timed GPU iterations validate result shape, submitted work and resource cleanup. Their `checksum` field is a shape-derived sentinel, not a numerical reduction. Every output cell is checked in the separate untimed verification sweep; adding readback to each resident iteration would measure a different workload.", ""]
for study, title in (("paired", "Frozen cancellable entry → closed entry"),
                     ("control", "Same binary: bounded closed owner → indivisible closed owner"),
                     ("ordinary", "Ordinary cancellable API: frozen → current"),
                     ("repeat", "Independent frozen-baseline replication"),
                     ("streaming", "Streaming fallback: both policies remain bounded")):
    values = pairs(data[study])
    lines += [f"### {title}", "", "| Tokens × width | Resident | Activation upload | All uploads |",
              "|---|---:|---:|---:|"]
    for m, d in sorted({key[:2] for key in values}, key=lambda shape: (shape[1], shape[0])):
        cells = []
        for mode in range(3):
            samples = values[m, d, mode]
            cells.append(f"{st.median(samples):+.2f}% ({sum(x > 0 for x in samples)}/{len(samples)} positive)")
        lines.append(f"| {m} × {d} | " + " | ".join(cells) + " |")
    lines.append("")
lines += ["The closed policy control changes only `closedSynchronous` in one executable. The ordinary API comparison checks that the architectural addition did not shift its cost unnoticed. Streaming disables prepared progression and has a separate full output sweep. The primary comparison includes the new owner and its lifecycle; it is not a claim that cancellable task execution became indivisible.", "",
          "## Fresh CPU and numerical-library comparison", "",
          "Shuffled fresh processes; absolute values are measured separately from the paired experiment. This is one unmasked, single-head, bias-free pre-RMSNorm transformer block with a 4× ReLU FFN, not full LLM generation. MPSGraph remains a benchmark reference, not a production dependency.", "",
          "| Tokens × width | Mode | CPU µs | Closed Metal µs | MPSGraph µs | Metal / MPS time |",
          "|---|---|---:|---:|---:|---:|"]
for m, d in shapes:
    for mode, label in enumerate(("resident", "activation", "all uploads")):
        cpu, gpu, mps = matched["cpu", m, d, 0], matched["closed", m, d, mode], matched["mps", m, d, mode]
        lines.append(f"| {m} × {d} | {label} | {cpu:.2f} | {gpu:.2f} | {mps:.2f} | {gpu / mps:.2f}× |")
lines += ["", "## Correctness and provenance", "",
          f"The native suite qualifies {qualification['tests']} tests, with {qualification['failed']} failures. Dedicated runner and GPU checks cover all four build/sanitizer modes, output ownership, every allocation-failure point, device failures, source deadlines, independent siblings and missing-proof/long-plan fallback.", "",
          "The initial full run exposed a late-observer race in the array-to-tensor cancellation fixture. A test-only gate now holds a real generated poll until its observer runs; ordinary arithmetic and production polling are unchanged. All four affected modes were rerun. Original failure logs remain in the qualification evidence, so the final coverage does not imply a clean first run.", "",
          "Both LSP completion fixtures also assumed `tra` could suggest only `transfer`; the existing `transpose` builtin makes that prefix ambiguous. Updated assertions cover both candidates and a distinct `transf` prefix, including lexical shadowing. Both modes were rebuilt and rechecked without changing completion implementation.", "",
          "Source, tensor-result and recovery fixtures assumed uploads always submitted blits. Shared uploads made their command ordinals target later work or wait for nonexistent submissions. They now arm a specific completion gate before execution and exercise both shared and private uploads. All 62 cases affected by fixture repairs were rechecked; production recovery behavior is unchanged.", "",
          "Full output comparisons cover 13 regular/odd shapes and all upload modes. Frozen, ordinary, bounded, closed and streaming outputs match bit-for-bit; independent CPU/MPSGraph numerical bounds also pass. Metal shader text/libraries and CPU LLVM are unchanged. Accepted timings disable profiling and Metal validation, and do not overlap builds, tests or profiling.", "",
          "Ordinary background applications remain active. External CPU-load samples are retained; this is not a dedicated idle lab machine. Pairing and replication reduce drift but do not erase all noise. The remaining device and dispatch gap needs separate profiling rather than arithmetic changes inferred from wall time alone.", "",
          "An initial calibration attempt was stopped by the load guard when the Codex renderer briefly used about four CPU cores (555% total external CPU). Its load record is retained separately; no completed paired samples from that attempt enter this report. The frozen artifacts and full output verification were reused after the spike subsided.", "",
          "## Evidence", ""]
lines += [f"- [{name}]({file})" for name, file in files.items()]
base = OUT / "gpu-closed-entry-2026-10-29"
base.with_suffix(".md").write_text("\n".join(lines) + "\n")
summary = dict(largest_resident_us=largest, largest_primary_reduction_percent=reduction,
               largest_repeat_reduction_percent=replication, largest_policy_reduction_percent=control,
               paired_median_reductions=[dict(m=m, d=d, mode=mode, percent=st.median(samples))
                                         for (m, d, mode), samples in paired.items()],
               ordinary_median_reductions=[dict(m=m, d=d, mode=mode, percent=st.median(samples))
                                           for (m, d, mode), samples in ordinary.items()],
               qualification=dict(tests=qualification["tests"], failed=qualification["failed"],
                                  skipped=qualification.get("skipped", [])),
               evidence_sha256={name: sha(OUT / file) for name, file in files.items()})
base.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11, "text.color": "#efedf4",
                     "axes.labelcolor": "#b9b4c5", "xtick.color": "#b9b4c5", "ytick.color": "#efedf4"})
fig, ax = plt.subplots(figsize=(13, 10), facecolor="#0d0c12")
ax.set_facecolor("#0d0c12")
primary = {tuple(row["cell"]): row["median_ns"] / 1000 for row in data["paired"]["summary"]}
y = np.arange(len(shapes))
for index, (label, color) in enumerate((("Previous cancellable entry", "#8792a2"),
                                       ("Closed synchronous entry", "#ad92ff"),
                                       ("MPSGraph reference", "#e7aa82"))):
    values = [matched["mps", m, d, 0] if index == 2 else primary["current" if index else "baseline", m, d, 0]
              for m, d in shapes]
    bars = ax.barh(y + (index - 1) * .25, values, height=.22, label=label, color=color)
    ax.bar_label(bars, labels=[f"{value:.0f} µs" for value in values], padding=5, fontsize=9)
ax.set_yticks(y, [f"{m} × {d}" for m, d in shapes])
ax.invert_yaxis()
ax.set_xlabel("Resident whole-block time (µs) · lower is faster", labelpad=18)
for spine in ax.spines.values():
    spine.set_visible(False)
ax.grid(axis="x", alpha=.14)
ax.set_axisbelow(True)
ax.set_xlim(0, ax.get_xlim()[1] * 1.12)
ax.legend(ncol=1, frameon=False, loc="upper right", fontsize=10)
fig.text(.08, .95, "GPU entry: fewer handoffs, identical arithmetic", fontsize=24, weight="bold")
fig.text(.08, .91, "Only isolated closed entries omit checkpoints · ordinary task APIs retain cancellation", color="#b9b4c5")
fig.text(.08, .075, "Apple M2 Max · 29 Oct 2026 · paired baseline/current; MPSGraph measured separately", fontsize=10, color="#b9b4c5")
fig.text(.08, .045, "Warm transformer block, not full LLM generation. All regressions and controls remain in the report.", fontsize=10, color="#b9b4c5")
fig.subplots_adjust(left=.15, right=.95, top=.85, bottom=.16)
fig.savefig(base.with_suffix(".png"), dpi=160)
fig.savefig(base.with_suffix(".svg"))
plt.close(fig)
print(base.with_suffix(".md"))
