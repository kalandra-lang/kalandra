"""Audit and render small-output contraction and guarded residual-fusion evidence."""
import collections,hashlib,json,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[3];out=root/'benchmarks/results'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
names=[f'transformer-attention-{k}-2026-10-29.json' for k in ('full-block','paired','profile','qualification','isolation','policy-screen')]
names += ['gpu-attention-rectangles-screen-2026-10-29.json','gpu-attention-rectangles-2026-10-29.json','transformer-attention-small-repeat-2026-10-29.json','transformer-attention-single-row-2026-10-29.json']
full,paired,profile,qualification,isolation,policy,screen,matrices,repeat,single=[json.loads((out/n).read_text()) for n in names]
assert paired['current_evidence_sha256']==sha(out/names[0])==profile['acceptance_sha256']==isolation['acceptance_sha256']
for data in (full,paired,profile,isolation,policy,screen,matrices,repeat,single):
 for name,value in data['artifact_sha256'].items():
  file=Path(name);assert sha(file if file.is_absolute() else root/file)==value,file
for name,value in full['source_sha256'].items():assert sha(root/name)==value,name
relations=collections.Counter(r['relation'] for r in paired['shader_source_relationship'].values())
assert relations['canonical input-load ABI swap plus epilogue extension']==1
assert sum(relations.values())==relations['identical']+1
base=root/'benchmarks/.build/gpu-attention-2026-10-29/baseline'
accept=root/'benchmarks/.build/gpu-attention-2026-10-29/acceptance'
assert sha(base/'cpu.ll')==sha(accept/'cpu.ll')
assert sha(base/'task.a')==sha(root/'build/native/native/libkalandra-task-core-optimized.a')
def groups(data):
 result=collections.defaultdict(list)
 for row in data['pairs']:result[tuple(row['group'])].append(row)
 return result
reduce=lambda rows:100*statistics.median(r['time_reduction'] for r in rows)
g=groups(paired);isolated=groups(isolation);mg=groups(matrices)
assert len(g)==27 and all(len(r)==5 for r in g.values())
assert len(mg)==16 and all(len(r)==5 for r in mg.values())
shapes=[(m,d) for d in (64,256,512) for m in (1,16,64)]
wins=sum(reduce(r)>0 for r in g.values());consistent=sum(all(x['time_reduction']>0 for x in r) for r in g.values())
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':'#eeedf2','axes.labelcolor':'#bbb7c9','xtick.color':'#aaa6b9','ytick.color':'#eeedf2','axes.edgecolor':'#292632','savefig.facecolor':'#0d0c12'})
fig,ax=plt.subplots(figsize=(12.8,8.8),facecolor='#0d0c12');ax.set_facecolor('#0d0c12')
for mode,color,label in [(0,'#ae97ff','Resident inputs'),(1,'#7f9cbf','Activation upload'),(2,'#e6aa82','All inputs uploaded')]:
 values=[reduce(g[m,d,mode]) for m,d in shapes];ys=[i+(mode-1)*.23 for i in range(9)]
 ax.barh(ys,values,height=.20,color=color,label=label)
 for y,v in zip(ys,values):ax.text(v+(.25 if v>=0 else -.25),y,f'{v:+.1f}%',va='center',ha='left' if v>=0 else 'right',fontsize=9)
ax.set_yticks(range(9),[f'{m} × {d}' for m,d in shapes]);ax.invert_yaxis()
ax.axvline(0,color='#797382',lw=.8);ax.grid(axis='x',color='#292632');ax.set_axisbelow(True)
ax.set_xlim(min(-4,min(reduce(r) for r in g.values())-3),max(12,max(reduce(r) for r in g.values())+5))
ax.set_xlabel('Reduction in complete execution time (%) · positive is faster',labelpad=14)
for s in ('top','right','left'):ax.spines[s].set_visible(False)
ax.legend(loc='upper left',bbox_to_anchor=(0,1.06),ncol=3,frameon=False,fontsize=10)
fig.text(.08,.94,'Kalandra GPU · attention tiles + residual fusion',fontsize=22,weight='bold')
fig.text(.08,.895,'Full transformer block against frozen 2fb73f53 · strict arithmetic and bounded cancellation',fontsize=11,color='#bcb6ca')
fig.text(.08,.065,'Five ABBA/BAAB blocks per case · process-median ratios · every result retained',fontsize=10,color='#bcb6ca')
fig.text(.08,.037,'Apple M2 Max · macOS 27 · 29 Oct 2026 · preparation and output readback excluded',fontsize=10,color='#bcb6ca')
fig.subplots_adjust(left=.16,right=.94,top=.84,bottom=.16)
stem=out/'gpu-attention-fusion-2026-10-29'
for ext in ('png','svg'):fig.savefig(stem.with_suffix('.'+ext),dpi=160)
def controls(m,d):return {r['backend']:r['median_ns'] for r in full['summary'] if (r['m'],r['d'],r['mode'])==(m,d,0)}
lines=['# Attention contraction parallelism and guarded residual fusion','',f'Against frozen `2fb73f53`, {wins}/27 full-block paired medians improve; {consistent} cases improve in every block. All negative results remain visible below. Numerical arithmetic remains strict.','',
'![All paired outcomes](gpu-attention-fusion-2026-10-29.png)','',
'## Implementation and semantics','',
'- Small-output matrices can select the existing contraction-parallel kernel: 3–64 rows, 8–64 columns, inner dimension 256–4096 and divisible by 32. The existing one/two-row wide profile is unchanged. No primitive numerical shader changes. The rule is based on dimensions and applies outside transformer programs; this tuning is physically measured on M2 Max only.',
'- The compiler places a nearby matrix result first in a map’s parameter ABI without changing expression operand/evaluation order. The runtime can fuse a proven non-faulting float32 map with one extra resident tensor, across an optional shape check that already passes from predicted output metadata. Failed/unavailable captures retain staged execution and fault precedence. Original generated programs remain compatible with the new runtime.',
'- The complete block uses 12 dispatches instead of 14; the large block uses six submissions instead of seven. Both computational operations are charged to the existing four-logical-stage / estimated 64 Mi-work quantum. Its existing oversized-single-fused-dispatch exception remains; no unrelated work is appended. Physical checkpoint placement changes within that contract. Products, float32 rounding and adjacent-pair reductions are unchanged; no FMA, precision relaxation or vendor numerical dependency is added.',
'- Compiler binary changes are confined to the Metal program emitter; full-block CPU LLVM and the task runtime archive are byte-identical. Six original stage sources remain identical. The residual map’s two canonical input loads swap ABI slots and gain an epilogue library; an exact source check proves that substitution preserves operand order. This is not a claim that every generated shader source is unchanged.',
'','See the [lifetime and cancellation design](../../docs/design/gpu-guarded-matrix-epilogues.md).','',
'## Complete-block acceptance','',
'Percentages are reductions in API time. Parentheses show positive blocks out of five. Pipeline loading, immutable-plan preparation and output readback are excluded; capture binding, device ACK, task observation and retirement remain inside timing. Transfers are included in their respective modes.','',
'| Rows × width | Resident | Activation upload | All seven uploads |','|---|---:|---:|---:|']
for m,d in shapes:
 vals=[f'{reduce(g[m,d,mode]):+.2f}% ({sum(r["time_reduction"]>0 for r in g[m,d,mode])}/5)' for mode in range(3)]
 lines.append(f'| {m} × {d} | '+' | '.join(vals)+' |')
lines+=['','## Fusion-only control','',
'Three separate paired blocks use exactly the same new shader bundle and generated header, with the attention policy enabled in both runtimes. Only residual-fusion support differs. The first policy-only transformer screen was mixed; direct matrix tests motivated keeping the selection policy. This control separates that policy from the broader fusion change.','',
'| Rows × width | Resident | Activation | All uploads |','|---|---:|---:|---:|']
for m,d in shapes:lines.append(f'| {m} × {d} | '+' | '.join(f'{reduce(isolated[m,d,mode]):+.2f}%' for mode in range(3))+' |')
lines+=['','## Small-case replication and rejected admission rule','',
'The first study showed a 3.18% loss in all five blocks for 1×256 activation upload, and a 4.35% resident loss at 1×512. A separate five-block run retained losses of 1.03% and 1.98% respectively, with mixed block signs. The largest-case resident reduction repeated at 12.78% in all five blocks. Small-workload parity is not established; the losses are retained, not dismissed as universally insignificant.',
'', 'An isolated three-block alternative disables only binary fusion for single-row outputs. It gives mixed results and does not fix the activation-upload case consistently, so it is rejected. Its unchanged large-case control also varies. Positive percentages in that control mean staging is faster; its signs must not be interpreted as gains over the original baseline.','',
'| Rows × width | Repeated baseline comparison: resident / activation / all | Staging instead of fusion: resident / activation / all |',
'|---|---:|---:|']
rg=groups(repeat);sg=groups(single)
assert len(rg)==len(sg)==12
for m,d in [(1,64),(1,256),(1,512),(64,512)]:
 lines.append(f'| {m} × {d} | '+' / '.join(f'{reduce(rg[m,d,mode]):+.2f}%' for mode in range(3))+' | '+' / '.join(f'{reduce(sg[m,d,mode]):+.2f}%' for mode in range(3))+' |')
lines+=['','## General matrix confirmation','',
'Five-block resident API tests verify every output cell against exact integer contractions divided by 64, outside timing. Boundary controls outside the new policy are retained. This is a matrix primitive result, not a claim of the same percentage improvement in a complete model.','',
'| M × K × N | Time reduction | Positive blocks |','|---|---:|---:|']
for (m,k,n),rows in sorted(mg.items()):lines.append(f'| {m} × {k} × {n} | {reduce(rows):+.2f}% | {sum(r["time_reduction"]>0 for r in rows)}/5 |')
lines+=['','## Fresh CPU/Metal/MPSGraph controls','',
'Equivalent bias-free unmasked single-head transformer block with pre-RMSNorm, four-times-width ReLU FFN and two residuals; this is not full model inference or generation. Vendor outputs meet the independent bound but need not share Kalandra’s reduction/FMA choices. MPSGraph remains benchmark-only; the wrapper encode count does not prove its internal submission count.','',
'| Rows × width | CPU µs | Kalandra GPU µs | MPSGraph µs | CPU / GPU | GPU / MPS |','|---|---:|---:|---:|---:|---:|']
for m,d in shapes:
 c=controls(m,d);lines.append(f'| {m} × {d} | {c["cpu"]/1000:.2f} | {c["gpu"]/1000:.2f} | {c["mps"]/1000:.2f} | {c["cpu"]/c["gpu"]:.2f}× | {c["gpu"]/c["mps"]:.2f}× |')
old=json.loads((out/'transformer-register-full-block-2026-10-28.json').read_text())
for data,label in [(old,'Previous'),(full,'Current')]:
 samples=[s for r in data['observations'] if (r['backend'],r['m'],r['d'],r['mode'])==('gpu',64,512,0) for s in r['samples'] if s['round']>=0]
 lines+=['',f'{label} large-case pipeline setup: {statistics.median(s["pipeline_setup_ns"] for s in samples)/1e6:.3f} ms; plan preparation: {statistics.median(s["preparation_ns"] for s in samples)/1000:.2f} µs. These setup observations come from separate studies, not a paired startup experiment.']
lines+=['','## Qualification','',
'- Optimized, debug, ASan/UBSan and TSan pass 1,745 APFloat cases / 1,769,284 cell/aggregate checks per mode. Attention domain edges, IEEE values, partial tiles and non-power-of-two reductions are included.',
'- Prepared graphs pass expanded binary/unary IEEE bit-pattern comparisons, noncommutative operand order, empty/failing guards, prior device-failure and cancellation precedence, original-input/alias safety, allocation-failure injection, every 4/6 submission boundary, concurrent calls, core teardown and low-budget/long-graph fallback. Generated source entry points run in all four modes.',
f'- CPU/Metal/MPS control validation: {len(full["verification"])} records / {sum(r["cells"] for r in full["verification"]):,} independent cell checks. All frozen and compatibility dumps are bit-identical. Numeric compiler, four-mode all-ten-type CLI programs and legacy graph batching passed. The CLI harness now accepts the shader-validation informational notice; numeric/exit checks remain strict.',
'- Diagnostic profiling is separate from acceptance. GPU command intervals and recorded encoding are not an exhaustive exclusive-time breakdown. No timing runs overlap builds, tests, profiling or plotting.','']
for m,d in [(1,512),(64,512)]:
 samples=[(r,s) for r in profile['observations'] if (r['m'],r['d'],r['mode'])==(m,d,0) for s in r['samples'] if s['round']>=0]
 med=lambda f:statistics.median(s[f]/r['iterations'] for r,s in samples)
 lines.append(f'- Profile {m} × {d}: wall {med("ns")/1e6:.3f} ms, GPU command intervals {med("gpu_ns")/1e6:.3f} ms, recorded encoding {med("encoding_ns")/1e6:.3f} ms; {med("submissions"):.0f} submissions / {med("dispatches"):.0f} dispatches.')
lines+=['','## Remaining work','',
'MPSGraph remains ahead and small blocks still favor CPU. The small-case losses remain open; the single-row staging control does not justify an extra admission rule. Next experiments should isolate invocation/transfer latency and test a snapshot-preserving unified-memory upload policy against matched vendor controls. Wider fusion must retain operand order, numerical rounding and the cancellation contract.','',
'## Evidence and reproduction','',
'Baseline artifacts were frozen before edits under `benchmarks/.build/gpu-attention-2026-10-29/baseline`; regenerate from `2fb73f53`. Raw JSON retains every calibration/sample, exact commands, source/artifact hashes and hardware/toolchain metadata. Rejected/noisy screening outcomes are retained alongside final acceptance. GPU numerical libraries are generated from the recorded compiler source.','']
for name in names:lines.append(f'- [{name}]({name}) — SHA256 `{sha(out/name)}`')
lines+=['','Reproduction entry points: `attention_policy.py`, `attention_rectangles.py`, `transformer_block.py`, `residual_execution.py` (with `--allow-epilogue-extension`), `residual_isolation.py` and `transformer_profile.py --dispatches 12 --large-submissions 6`. Use the exact arguments recorded in each JSON; regenerate ignored artifacts before auditing/rendering.']
stem.with_suffix('.md').write_text('\n'.join(lines)+'\n')
stem.with_suffix('.json').write_text(json.dumps(dict(evidence_sha256={n:sha(out/n) for n in names},positive_medians=wins,all_positive_blocks=consistent,cpu_llvm_sha256=sha(base/'cpu.ll'),renderer_sha256=sha(Path(__file__))),indent=2)+'\n')
print(stem.with_suffix('.md'))
