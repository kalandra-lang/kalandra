"""Audit final AI evidence and draw a factual CPU/GPU/library comparison."""
import hashlib,json,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
import numpy as np
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'benchmarks/results'
NAMES=dict(cpu='ai-cpu-final',gpu='ai-gpu-final',library='ai-ffn-cpu-library-final',model='transformer-block-performance',profile='transformer-block-profile',stages='transformer-stages-qualification')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
data={k:json.loads((OUT/(v+'-2026-10-28.json')).read_text()) for k,v in NAMES.items()}
measured=json.loads((OUT/'transformer-runner-measured-source-2026-10-28.json').read_text())
audit=[]
for name,record in data.items():
    checked=0
    for field in ('source_sha256','artifact_sha256'):
        for file,digest in record[field].items():
            path=Path(file);path=path if path.is_absolute() else ROOT/path
            if field=='source_sha256' and file in measured['source_snapshots']:
                assert hashlib.sha256(measured['source_snapshots'][file].encode()).hexdigest()==digest==measured['source_sha256'][file]
            else:assert path.is_file() and sha(path)==digest,(name,field,file)
            checked+=1
    audit.append(dict(dataset=name,path=NAMES[name]+'-2026-10-28.json',sha256=sha(OUT/(NAMES[name]+'-2026-10-28.json')),checked_sources_artifacts=checked))
def paired(kind,group):
    values=data[kind]['paired']
    rows=[r for r in values['pairs'] if tuple(r['group'])==tuple(group)]
    assert len(rows)==5
    median=lambda field:statistics.median(r[field] for r in rows)
    return median('baseline_ns'),median('current_ns'),100*median('time_reduction')
def model(backend,m,d,mode=0):return next(r['median_ns'] for r in data['model']['summary'] if (r['backend'],r['m'],r['d'],r['mode'])==(backend,m,d,mode))
cases=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
cpu_cases=sorted({tuple(r['group']) for r in data['cpu']['paired']['pairs']},key=lambda x:(x[1],x[0]))
gpu_cases=[(1,256),(16,256),(128,256),(1,1024),(32,1024),(128,1024),(1,2048),(32,2048)]
profile=[]
for m in (1,64):
    rows=[r for r in data['profile']['observations'] if r['m']==m and r['mode']==0]
    metrics={k:statistics.median(s[k]/r['iterations'] for r in rows for s in r['samples'] if s['round']>=0) for k in ('ns','gpu_ns','encoding_ns','submissions','dispatches','attempts')}
    assert metrics['gpu_ns']>0 and metrics['encoding_ns']>0 and metrics['submissions']==16 and metrics['dispatches']==16
    profile.append(dict(m=m,d=512,**metrics))
bg='#0c0c10';fg='#ededf0';muted='#a7a5b1';purple='#b298ff';gray='#8792a3';peach='#e8ac84'
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'figure.facecolor':bg,'axes.facecolor':bg,'axes.edgecolor':'#37353e','text.color':fg,'axes.labelcolor':muted,'xtick.color':muted,'ytick.color':fg,'savefig.facecolor':bg})
fig=plt.figure(figsize=(16,11));grid=fig.add_gridspec(2,2,height_ratios=(1,1.25),hspace=.52,wspace=.32,left=.075,right=.96,top=.83,bottom=.16)
fig.text(.075,.94,'Kalandra CPU and GPU — verified AI workloads',fontsize=26,weight='bold')
fig.text(.075,.905,'Apple M2 Max · macOS 27 · 28 October 2026 · warm APIs including completion and cleanup',color=muted,fontsize=13)
for axis,kind,groups,title in [(fig.add_subplot(grid[0,0]),'cpu',[(1,256),(1,1024),(1,2048),(16,1024),(128,1024)],'CPU FFN: current / frozen baseline'),(fig.add_subplot(grid[0,1]),'gpu',[(0,128,256),(0,32,1024),(0,128,1024),(0,32,2048),(0,1,2048)],'Resident GPU FFN: current / baseline')]:
    for i,group in enumerate(groups):
        before,after,drop=paired(kind,group);ratio=after/before
        axis.barh(i+.14,1,height=.23,color=gray,label='Baseline' if i==0 else None)
        axis.barh(i-.14,ratio,height=.23,color=purple if drop>=0 else peach,label='Current' if i==0 else None)
        axis.text(max(1,ratio)+.035,i-.14,f'{after/1000:,.1f} µs',va='center',fontsize=10)
    axis.set_yticks(range(len(groups)),[f'{g[-2]} × {g[-1]}' for g in groups]);axis.invert_yaxis();axis.axvline(1,color=muted,alpha=.4,ls='--');axis.set_xlim(0,1.5);axis.set_title(title,loc='left',pad=18,weight='bold',fontsize=14);axis.set_xlabel('Relative time · lower is faster');axis.grid(axis='x',alpha=.12);axis.set_axisbelow(True)
    handles,labels=axis.get_legend_handles_labels()
    if kind=='gpu':handles.append(Patch(color=peach));labels.append('Current regression')
    axis.legend(handles,labels,frameon=False,loc='upper right',fontsize=10)
    for side in ('top','right','left'):axis.spines[side].set_visible(False)
axis=fig.add_subplot(grid[1,:]);x=np.arange(len(cases));width=.23
for offset,name,color,label in [(-1,'cpu',gray,'Kalandra CPU'),(0,'gpu',purple,'Kalandra Metal'),(1,'mps',peach,'MPSGraph control')]:
    vals=[model(name,m,d)/1000 for m,d in cases];axis.bar(x+offset*width,vals,width,color=color,label=label)
axis.set_yscale('log');axis.set_xticks(x,[f'{m} × {d}' for m,d in cases]);axis.set_ylabel('Microseconds · logarithmic scale');axis.set_title('Complete pre-RMSNorm transformer block · resident inputs',loc='left',fontsize=15,weight='bold',pad=18);axis.legend(frameon=False,ncol=3,loc='upper left',fontsize=11);axis.grid(axis='y',alpha=.13);axis.set_axisbelow(True)
for side in ('top','right'):axis.spines[side].set_visible(False)
fig.text(.075,.085,'Five counterbalanced blocks; every complete output matrix independently checked. Separate products / adjacent-pair trees retained.',color=muted,fontsize=10)
fig.text(.075,.061,'MPSGraph is a benchmark-only FP32 library control; fusion / FMA / reassociation allowed and checked with propagated error bounds.',color=muted,fontsize=10)
fig.text(.075,.037,'Single head, no mask, unit RMS gain, 4× ReLU FFN. This measures a block, not tokenizer / KV cache / full-model generation.',color=muted,fontsize=10)
base=OUT/'native-ai-performance-2026-10-28';fig.savefig(base.with_suffix('.png'),dpi=160);fig.savefig(base.with_suffix('.svg'));plt.close(fig)
lines=['# Native AI performance — 28 October 2026','',
'The five-stage sequence is implemented and locally qualified on Apple M2 Max / macOS 27. All timings are warm APIs; setup, process startup and pipeline/graph preparation are excluded. New Windows/Linux CPU gates are configured, but their hosted results are separate from these Mac measurements.','',
'- CPU single-row residual FFNs take 80.2%, 84.5% and 88.4% less time at D=256/1024/2048 versus the frozen starting compiler.',
'- Larger resident GPU FFNs improve 6.0–24.0%. Small regressions remain, including 6.9% at 1×2048; this is not a universal GPU improvement.',
'- A complete transformer block now runs from Kalandra source on CPU and Metal. Resident Metal beats Kalandra CPU 1.34× at 16×512 and 2.71× at 64×512.',
'- Metal takes about 7.5–9.5× the MPSGraph time in resident cases. Optimized CPU libraries also remain ahead on most FFN shapes.',
'- No new universal C++/Rust ranking is inferred from tensor builtins. The ordinary affine/polynomial/GCD/mixed/array controls retain byte-identical generated LLVM IR versus the frozen compiler.','',
'![Verified AI benchmark comparison](native-ai-performance-2026-10-28.png)','',
'## What changed','',
'CPU short-row kernels stream adjacent RHS columns through separately rounded register subtrees and bounded root storage. Batched float32 kernels use four rows by four columns; float64 retains its previous tile. General layouts, unsupported crossover shapes and odd tails retain checked fallbacks. No FMA/reassociation was introduced.',
'',
'The legacy GPU graph donates only private, last-use, same-shape map backing and combines an eligible tail reduction within the existing four-stage / 64-Mi estimated-work batch allowance. Submitted resources remain retained through ACK, including failure, deadline and cancellation.',
'',
'New rank-two primitives are `transpose`, `softmaxRows`, and `rmsNormRows`. Transpose preserves all ten numeric types bit-for-bit. CPU row math supports hardware float32/float64; Metal row math supports hardware float32. Unsupported software row arithmetic / GPU float64 row math reject explicitly. A typed float32 bridge reuses the independently qualified native Metal matrix profiles. Larger source matrix graphs outline checked helpers instead of relaxing the conservative stack budget.',
'',
'## CPU residual FFN','',
'`Y = ReLU(X W_up) W_down + X`, including invocation ownership, packing, intermediates and final adjacent-tree scalar observation. Percentages use median paired time reductions; elapsed values below use the median pair endpoints. Packing is charged and separately accounted as a K×N allocation; no isolated packing-cycle percentage is claimed.','',
'| Rows × width | Baseline µs | Current µs | Less time |','|---|---:|---:|---:|']
for m,d in cpu_cases:
    a,b,drop=paired('cpu',(m,d));lines.append(f'| {m} × {d} | {a/1000:,.2f} | {b/1000:,.2f} | {drop:.2f}% |')
lines+=['','CPU library controls use the same data and equation, with their ordinary optimized FP32 accumulation. They permit FMA/reassociation and are checked against a float64 reference with propagated gamma bounds. Accelerate is reference-only; Kalandra does not link it.','',
'| Rows × width | Kalandra µs | OpenBLAS 1 thread µs | OpenBLAS 12 threads µs | Accelerate 1 thread µs | Accelerate AUTO µs |','|---|---:|---:|---:|---:|---:|']
lib=data['library']['measurement']['summary']
for m,d in gpu_cases:
    ns=lambda kind,mode:next(r['median_ns']/1000 for r in lib if r['cell']==[kind,mode,m,d])
    lines.append(f'| {m} × {d} | '+ ' | '.join(f'{ns(kind,mode):,.2f}' for kind,mode in [('cpu',1),('openblas',1),('openblas',12),('accelerate',1),('accelerate',0)])+' |')
lines+=['','## GPU residual FFN','',
'Same frozen compiler-emitted shaders and task archive around old/new production Metal runtimes. Scalar result readback is included. Percentages are paired time reductions, with negative values retained as regressions.','',
'| Rows × width | Resident baseline µs | Resident current µs | Resident less time | Activation upload less time | All uploads less time |','|---|---:|---:|---:|---:|---:|']
for m,d in gpu_cases:
    a,b,drop=paired('gpu',(0,m,d));lines.append(f'| {m} × {d} | {a/1000:,.2f} | {b/1000:,.2f} | {drop:.2f}% | {paired("gpu",(1,m,d))[2]:.2f}% | {paired("gpu",(2,m,d))[2]:.2f}% |')
lines+=['','## Complete transformer block','',
data['model']['model'],'',data['model']['timing'],'',
'The source has distinct hashed Q/K/V/output/FFN weights. All 13 verification shapes (nine timed and four odd/non-dyadic) pass complete per-cell checks on CPU, Metal and MPSGraph in all residency modes. Eight observable source prefixes independently verify normalized inputs, Q/K/V, attention probabilities, attention residual, second normalization and FFN hidden output. The norm/Q/K/V prefix outputs are bit-exact against the separately rounded reference; later outputs use propagated math/rounding bounds.','',
'| Sequence × width | CPU µs | Metal µs | MPSGraph µs | CPU / Metal speedup | Metal / MPS time |','|---|---:|---:|---:|---:|---:|']
for m,d in cases:
    a,b,c=(model(name,m,d)/1000 for name in ('cpu','gpu','mps'));lines.append(f'| {m} × {d} | {a:,.2f} | {b:,.2f} | {c:,.2f} | {a/b:.2f}× | {b/c:.2f}× |')
lines+=['','The MPSGraph framework emitted `Failed to import MPS module` during preparation. The diagnostic is retained in raw evidence; execution succeeded through the explicitly supplied Metal command buffer/device, and all numerical checks passed. No internal MPS dispatch count is claimed.','',
'## Remaining architectural gap','',
'Separate instrumented runs retain the same frozen kernels/runtime archives and explicitly enable nonzero runtime counters. These are diagnostic times, not acceptance timings. Recorded encoding does not include every helper/setup cost and the remaining wall time is not assigned wholly to one subsystem.','',
'| Sequence × width | Instrumented wall µs | GPU command time µs | Recorded encoding µs | Submissions | Dispatches | Allocation/admission checkpoints |','|---|---:|---:|---:|---:|---:|---:|']
for r in profile:lines.append(f'| {r["m"]} × 512 | {r["ns"]/1000:,.2f} | {r["gpu_ns"]/1000:,.2f} | {r["encoding_ns"]/1000:,.2f} | {r["submissions"]:.0f} | {r["dispatches"]:.0f} | {r["attempts"]:.0f} |')
lines+=['',
'The next change needs a prepared typed GPU execution plan with bounded command chunks and ownership-aware temporary reuse. Sixteen per-stage ACK boundaries and 235 allocation/admission checkpoints remain even with preloaded kernels. Batch admission must preserve checked-operation first faults, stop at device-dependent/error-producing boundaries, and retain all owners/fault flags through real completion; the successful legacy graph optimization cannot simply be copied across arbitrary typed operations. Cancellation and strict arithmetic should stay intact. After reducing lifecycle costs, native matrix/row kernels still need separate GPU-time work to close the library gap.',
'',
'## Qualification and reproducibility','',
'CPU/GPU tensor operations passed optimized, debug, ASan/UBSan and TSan checks, including empty axes, general CPU strides, input immutability, numerical extremes, allocation injection, mid-loop CPU cancellation, held GPU cancellation/deadline/device failure and cleanup. The typed float32 matrix bridge passed 1,724 APFloat cases and 1,598,840 output checks per mode. Existing source regression suites and four typed matrix program/control gates passed. Authored/rendered docs verified all 68 code blocks (36 CPU programs, three GPU programs, plus classified module/setup examples).',
'',
'No frequency pinning, thermal control, affinity or desktop isolation is claimed. Other physical devices/platforms need their own qualification. Historical screen/repeat reports preserve losing candidates and timing variability; final reports do not overwrite them. Large executable/IR/toolchain artifacts remain in ignored experiment storage, not tracked benchmark results.','',
'Raw final evidence:','']
for key,name in NAMES.items():lines.append(f'- [{name}-2026-10-28.json]({name}-2026-10-28.json)')
lines+=['','Source/artifact digests were rechecked by `native/benchmarks/fair/ai_report.py`; the audit and plotted inputs are saved beside this report.']
base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
base.with_suffix('.json').write_text(json.dumps(dict(audit=audit,profile=profile,cpu_pairs=[dict(m=m,d=d,baseline_ns=paired('cpu',(m,d))[0],current_ns=paired('cpu',(m,d))[1],less_time_percent=paired('cpu',(m,d))[2]) for m,d in cpu_cases],script_sha256=sha(Path(__file__))),indent=2)+'\n')
print(base.with_suffix('.md'))
