"""Audit the complete FFN runs and produce an aggregate report from raw samples."""
import json
from pathlib import Path
import statistics
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import residual_ffn as f
import protocol as p

root=p.ROOT/'benchmarks/results'
files=[root/'residual-ffn-2026-10-28.json',root/'residual-ffn-repeat-2026-10-28.json']
aggregate={};audits=[]
for file in files:
    data=json.loads(file.read_text());measurement=data['measurement']
    assert data['oracle_self_test']['corruption_rejected']
    assert len(data['verification'])==96
    assert all(r['strict_bit_match'] for r in data['verification'] if r['backend'] in ('cpu','gpu'))
    assert all(0<=r['max_bound_fraction']<=1 for r in data['verification'])
    assert all(p.sha(p.ROOT/name)==digest for name,digest in data['artifact_sha256'].items())
    assert all(p.sha(p.ROOT/name)==digest for name,digest in data['source_sha256'].items())
    expected={(r['backend'],r['mode'],r['m'],r['d']):r['expected'] for r in data['verification'] if r['pattern']==0}
    samples=measurement['observations']+measurement['calibration']
    for row in samples:
        p.observations('\n'.join(json.dumps(s) for s in row['samples']),expected[tuple(row['cell'])],row['iterations'],measurement['protocol']['rounds'] if row in measurement['observations'] else 3)
    summaries=measurement['summary']
    assert {tuple(r['cell']) for r in summaries}=={(b,mode,m,d) for b,mode in f.BACKENDS for m,d in f.CASES}
    for row in summaries:
        observed=[r for r in measurement['observations'] if r['cell']==row['cell']]
        assert len(observed)==3 and {r['process'] for r in observed}=={0,1,2}
        medians=[statistics.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0) for r in observed]
        assert medians==row['process_medians_ns'] and statistics.median(medians)==row['median_ns']
        aggregate.setdefault(tuple(row['cell']),[]).extend(medians)
    audits.append(dict(file=p.key(file),sha256=p.sha(file),verified_output_cells=sum(r['cells'] for r in data['verification']),verified_batches=len(samples),measured_processes=len(measurement['observations'])))
times={k:statistics.median(v) for k,v in aggregate.items()}
assert all(len(v)==6 for v in aggregate.values())
lines=['# Residual feed-forward CPU/GPU and numerical-library comparison','',
'Two complete runs on the current committed compiler (`a7e82604`, evidence commit `87a456fd`), Apple M2 Max/macOS 27.2. This is a numerical API comparison, not a source-language ranking or full LLM benchmark. No production compiler/runtime code changed.','',
'## Workload and timing boundary','',
'`Y = ReLU(X W_up) W_down + X`, with shapes M×D, D×4D and 4D×D. The weights are distinct, dense, runtime inputs generated with an integer hash; columns do not deliberately repeat. This is a bias-free residual feed-forward block, followed by a scalar sum to observe the complete result. It omits attention, normalization, KV caches, tokenization and model generation.','',
'Kalandra CPU executes compiled source. Metal executes the source-generated shaders and exactly the product graph emitted by the compiler, through the production managed runtime. Fresh invocation/tasks, output/intermediate allocation, completion and cleanup are timed. Setup, process startup, input generation, shader preparation and library loading are excluded. GPU scalar completion/readback is timed; full output downloads happen only in untimed validation.','',
'The GPU driver uses one host-driven task worker and a per-runtime payload budget of max(64 MiB, 128 D² + 128 M D bytes) to accommodate larger weights, host/device copies and intermediates. Production defaults are unchanged. Runtime payload peaks exclude the driver-owned input vectors and vendor-library private workspace.','',
'GPU resident excludes initial input uploads. GPU activation includes the activation upload with persistent weights. GPU all-upload includes all three uploads per call; it models a weight-reuploading application, not an unavoidable inference cost. CPU library inputs are already in host memory. BLAS may use FMA and different contraction order; Kalandra keeps its separately rounded products and adjacent-pair tree. Accelerate is reference-only and adds no production dependency. OpenBLAS 1/12 thread counts and OpenMP limits are checked; Accelerate SINGLE/AUTO are set and queried, with no fixed AUTO thread-count claim. These are CPU-library controls: no optimized vendor-GPU library baseline was measured.','',
'## Results','',
'Each run uses three independent processes, two warmups and five measured adaptive batches per process, targeting 30 ms. Cases/backend order is shuffled. Values below are milliseconds per call, aggregating all six process medians across both complete runs. All observations, including slower runs, are retained. No builds, sampling or plots ran concurrently with acceptance measurements. Desktop activity, affinity and clocks were not isolated.','',
'| M × D | K CPU | GPU resident | GPU activation | GPU all-upload | OpenBLAS 1 | OpenBLAS 12 | Accelerate 1 | Accelerate AUTO |',
'|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for m,d in f.CASES:
    values=[times[(b,mode,m,d)]/1e6 for b,mode in f.BACKENDS]
    lines.append('| '+f'{m} × {d}'+' | '+' | '.join(f'{v:.3f}' for v in values)+' |')
lines+=['','### Repeat variation','',
'GPU timings vary materially between processes. Each entry below shows the complete first-run and repeat-run resident medians, rather than substituting the faster one.','',
'| M × D | Run 1 GPU resident ms | Run 2 GPU resident ms | Six-process CPU / GPU resident speedup |','|---|---:|---:|---:|']
runs=[json.loads(file.read_text()) for file in files]
for m,d in f.CASES:
    v=[next(r['median_ns'] for r in run['measurement']['summary'] if r['cell']==['gpu',0,m,d])/1e6 for run in runs]
    lines.append(f'| {m} × {d} | {v[0]:.3f} | {v[1]:.3f} | {times[("cpu",1,m,d)]/times[("gpu",0,m,d)]:.2f}× |')
lines+=['','## Correctness','',
'Before each study, all eight backend/mode variants compute complete output matrices for every timed shape, plus odd 3×7, 2×33 and 1×257 controls. Both dyadic and non-dyadic inputs are included in qualification. Per study, 2,159,464 output cells are checked. CPU and all Metal modes match an independent binary32 product/pair-tree oracle bit for bit; the oracle uses NumPy elementwise arithmetic, not BLAS. A separate scalar implementation checks 84 product/tree cells and a deliberately corrupted output is rejected.','',
'Optimized-library cells are checked against a float64 reference with forward-error bounds propagated through both GEMMs, ReLU and the residual addition. The maximum observed error is under 0.8% of that conservative bound. Each backend scalar must match a separately recomputed tree sum of its complete output cells. Every warmup, calibration and measured batch checksum is checked against that independently qualified backend result. Kalandra CPU invocation allocations/releases and GPU task groups, retained owners, payload and in-flight resources are checked for retirement. This benchmark qualification does not replace IEEE special-value, fault or cancellation conformance tests.','',
'## Separate profiling and next work','',
'Owned-child 2-second CPU samples place 1,666 of 1,673 main-thread samples in the two outlined matrix kernels for the 128×1024 case. The 1×256 and 1×1024 cases spend almost all samples in the source function containing the narrow matrix loops; allocation paths contribute only a handful of observations. Sampling counts locate work, not exact exclusive-time percentages.','',
'Inspection of `native/lib/Backend.cpp` explains a concrete shape distinction: fewer than four rows bypass the wide kernel. Its dense-right packing additionally requires at least 32 rows. Single-row execution therefore uses the narrower 4/2/1-column path with contraction scratch and full row-pitch RHS access. A dedicated streaming GEMV path that preserves the adjacent reduction tree is the first CPU experiment to make. The larger batched case already uses packed wide kernels; its remaining gap needs a separate packing-versus-kernel study, not more invocation tuning.','',
'GPU diagnostic timestamps are separate from acceptance timing. The 1×256 resident diagnostic records 0.795 ms wall time, 0.175 ms summed GPU command durations and 0.040 ms CPU encoding, with two submissions and five dispatches. The 128×1024 case records 3.119 ms wall, 1.828 ms GPU and 0.079 ms encoding, with five submissions and seven dispatches. These intervals are not an additive decomposition: the difference includes scheduling, waits and completion and cannot be called pure launch overhead. The four-logical-stage / 64-Mi estimated-work batching contract remains intact.','',
'The next GPU experiments are device-resident tensor continuations, fewer managed submission boundaries where cancellation/work budgets permit, and buffer lifetime/reuse planning. Upload-inclusive results make weight residency a priority. A full transformer and an optimized vendor-GPU control remain necessary before claiming LLM-level or GPU-library parity.','',
'## Reproduction and evidence','',
'```sh','python3 native/benchmarks/residual_ffn.py --output benchmarks/results/NEW.json',
'python3 native/benchmarks/residual_ffn.py --reuse-build --output benchmarks/results/NEW-REPEAT.json',
'python3 native/benchmarks/residual_ffn.py --reuse-build --profile-only --output benchmarks/results/NEW-PROFILE.json',
'python3 native/benchmarks/fair/ffn_report.py','```','',
'The runner requires the pinned native/Metal build, NumPy and Homebrew OpenBLAS on this Mac. Use unused output paths; `ffn_report.py` audits the recorded October 28 files and regenerates this report. Physical Metal access is required. Generated artifacts stay in ignored `benchmarks/.build/residual-ffn`. Source snapshots, compiler/library/artifact hashes, actual commands, system versions and every timing sample are retained.','',
'- [Run 1](residual-ffn-2026-10-28.json)','- [Complete repeat](residual-ffn-repeat-2026-10-28.json)',
'- [GPU diagnostics](residual-ffn-gpu-profile-2026-10-28.json)','- [Owned-process CPU diagnostics](residual-ffn-cpu-profile-2026-10-28.json)',
'- [Audit](residual-ffn-audit-2026-10-28.json)','']
(root/'residual-ffn-2026-10-28.md').write_text('\n'.join(lines))
(root/'residual-ffn-audit-2026-10-28.json').write_text(json.dumps(dict(audits=audits,summary=[dict(cell=list(k),median_ns=times[k],process_medians_ns=v) for k,v in aggregate.items()],auditor_sha256=p.sha(__file__)),indent=2)+'\n')
print('\n'.join(lines[16:30]))
