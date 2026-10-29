"""Audit whole-model experiments, keeping failed promotion gates and diagnostic limits."""
import collections,hashlib,json,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results';evidence={}
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
def read(name):
 path=OUT/name;data=json.loads(path.read_text());evidence[name]=sha(path)
 for field in ('source_sha256','artifact_sha256','source_artifact_sha256'):
  for file,digest in data.get(field,{}).items():assert sha(ROOT/file)==digest,file
 return data
def profiles(name):
 d=read(name);assert d['acceptance'] is False;groups=collections.defaultdict(lambda:collections.defaultdict(list))
 for row in d['observations']:
  for key in ('ns','gpu_ns','encoding_ns'):
   groups[tuple(row['cell'])][key].append(st.median(s[key]/row['iterations']/1000 for s in row['samples'] if s['round']>=0))
 return [dict(cell=list(cell),**{k:st.median(v) for k,v in values.items()}) for cell,values in sorted(groups.items(),key=str)]
assert sha(ROOT/'native/benchmarks/independent_dispatch_profile.py')==sha(Path('/private/tmp/kalandra-independent-profile.py'))
q=read('gpu-independent-dispatch-verification-2026-10-29.json');assert len(q['checks'])==117 and all(r['bit_identical'] for r in q['checks'])
readonly=read('gpu-independent-readonly-verification-2026-10-29.json');assert len(readonly['checks'])==234 and all(r['bit_identical'] for r in readonly['checks'])
read('gpu-independent-dispatch-budget-2026-10-29.json')
access=read('gpu-independent-readonly-access-2026-10-29.json');assert [r['arguments'][0]['access'] for r in access['records']]==[1,0]
studies=[]
for kind in ('frozen','control'):
 d=read(f'gpu-independent-dispatch-{kind}-2026-10-29.json');assert d['verification_sha256']==evidence['gpu-independent-dispatch-verification-2026-10-29.json']
 grouped=collections.defaultdict(list)
 for row in d['pairs']:grouped[tuple(row['group'])].append(row)
 assert len(grouped)==12 and all(len(v)==7 for v in grouped.values())
 cells=[]
 for cell,rows in sorted(grouped.items()):
  cells.append(dict(cell=list(cell),baseline_us=st.median(r['baseline_ns'] for r in rows)/1000,current_us=st.median(r['current_ns'] for r in rows)/1000,reduction_percent=100*st.median(r['time_reduction'] for r in rows),winning_blocks=sum(r['time_reduction']>0 for r in rows),block_reductions=[100*r['time_reduction'] for r in rows]))
 load=read(f'gpu-independent-dispatch-{kind}-2026-10-29-load.json');values=[r['external_cpu_percent'] for r in load['samples']]
 studies.append(dict(study=kind,cells=cells,load=dict(median=st.median(values),maximum=max(values),samples=len(values))))
diag={name:profiles(f'gpu-independent-{name}-2026-10-29.json') for name in ('dispatch-profile','dispatch-arena-profile','readonly-profile')}
lines=['# Independent projection scheduling: whole-model gate','',
 'Apple M2 Max, 29 October 2026. **No production policy is promoted.** Isolated concurrent Q/K/V gains did not establish a whole-transformer improvement. The copied-runtime candidate remains an experiment; no language, cancellation, arithmetic, resource-limit or public ABI contract changes. All unfavorable outcomes are retained.','',
 '## Frozen and same-binary controls','',
 'The candidate groups three independent register16 float32 projections in one concurrent encoder, conditional on the existing closed synchronous proof. Shape guards remain in place; matching requires ready same-device operands, the same left operand and compatible shapes/kernel identity. Each output still uses the existing allocation/ownership machinery. Ordinary and bounded paths retain their original policy.','',
 'Seven ABBA/BAAB blocks per cell use 50 ms adaptive batches with a 512-invocation cap. Frozen compares the previously qualified binary with the prototype; control disables/enables grouping in the same prototype binary. Modes are 0: resident inputs, 1: fresh input upload, 2: fresh input/weight uploads. Shapes 1×512 and 64×256 do not group and are inactive controls. Positive reductions are faster. Paired medians are not ratios of the displayed timing medians.','',
 '| Study | Rows×width | Mode | Baseline µs | Candidate µs | Time reduction | Winning blocks |','|---|---|---:|---:|---:|---:|---:|']
for study in studies:
 for cell in study['cells']:
  m,d,mode=cell['cell'];lines.append(f"| {study['study']} | {m}×{d} | {mode} | {cell['baseline_us']:.2f} | {cell['current_us']:.2f} | {cell['reduction_percent']:+.2f}% | {cell['winning_blocks']}/7 |")
lines+=['','Background activity and large block-to-block changes affect active and inactive cells. These are screening results, not uncontended acceptance. Their exact regression magnitudes should not be generalized. No local tests/builds/profiling overlapped the timed studies. Sampled external CPU load uses 100% per core:','']
for study in studies:
 l=study['load'];lines.append(f"- {study['study']}: median {l['median']:.1f}%, maximum {l['maximum']:.1f}% ({l['samples']} samples).")
lines+=['','## Correctness and limits','',
 '117 full-transformer cases pass: 13 shapes, three execution policies and three upload modes. Every final float32 bit matches the frozen baseline and passes the independent propagated-error oracle. Odd/inactive shapes and the active group trace are checked with Metal API/shader validation. This is not a new full native-suite qualification. Allocation-failure, cancellation and resource-bound promotion gates remain pending for the copied runtime. Timed calls validate shape, submission counts and resource cleanup, **not a numerical checksum**; full numerical checks are separate.','',
 'An initial 1024-wide whole-model attempt fails in the frozen baseline before candidate execution. A diagnostic of both binaries identifies `Error::Budget` (code 6) under the existing default 64 MiB budget. The original failed attempt and both probes are retained. This does not indicate a scheduling regression; isolated projections fit a different resource footprint.','',
 '## GPU command versus CPU encoding diagnostics','',
 '`Options.profile` records GPU command duration and CPU encoding duration. These measurements are separate from unprofiled screening and are not acceptance. GPU timestamp intervals are not occupancy or register measurements. The first diagnostic places most of the large candidate penalty inside the GPU command, rather than in CPU encoding. Separate parameter buffers do not eliminate the large-case penalty, so a shared parameter arena alone is not an adequate explanation.','']
for name,rows in diag.items():
 lines += [f'### {name}','', 'Cell fields: version, rows, width; final field is arena enabled for the arena control, or concurrent enabled for the read-only control. In the read-only control baseline/current mean writable/read-only declarations, both recompiled identically. Otherwise they mean grouping disabled/enabled. Values are medians of process medians, in µs.','', '| Cell | Wall | GPU command | CPU encoding |','|---|---:|---:|---:|']
 for row in rows:lines.append(f"| {', '.join(map(str,row['cell']))} | {row['ns']:.2f} | {row['gpu_ns']:.2f} | {row['encoding_ns']:.2f} |")
 lines.append('')
lines+=['## Read-only shader control','',
 'Pipeline reflection originally reports matrix inputs and shape parameters as read-write, although register16 only reads them. A copied shader adds `const` to those three arguments only; output remains writable. Both control and candidate bundles are recompiled with the same pinned compiler and strict options. 234 additional full-output cases (117 per bundle) match the original GPU bits and independent oracle. Reflection confirms read-only access for both inputs and parameters, with output still read-write. Serial timings remain comparatively close; concurrent results have large swings, including severe regressions. Read-only declarations do not reliably recover the lost performance. Reflection/access metadata alone does not prove a driver scheduling or hazard mechanism. The profile compares declaration variants in both serial and concurrent modes; it does not qualify a production optimization.','',
 '## Decision and next gate','',
 'Keep production scheduling and shaders unchanged. The isolated batched shader remains a distinct option: it places independent projections into one grid, rather than relying on concurrent-encoder scheduling. It still needs a copied whole-model integration and the complete ownership/fault/budget/cancellation gates. Do not relax strict reduction order or resource limits to recover a speedup. The exact cause of the concurrent whole-model penalty is not established by these controls.','',
 '## Reproduction and evidence','',
 'Build the copied runtime with `native/benchmarks/independent_dispatch_prototype.py --build PATH`, then run `independent_dispatch_verify.py` and sequential `independent_dispatch_measure.py --study frozen|control`. The profile driver builder is `independent_dispatch_profile_prepare.py`; the three diagnostic scripts use the dated frozen paths recorded in their source. `independent_readonly_probe.py prepare`, `verify`, then `measure` creates same-compiler shader controls. Prerequisite frozen program/bundle/task artifacts come from the closed-entry and attention studies. Never overlap timing with builds, tests or profiling. The initial profile collector was run from a temporary path; its committed `independent_dispatch_profile.py` copy has identical bytes. Generated binaries, shader bundles and captures stay ignored; tracked records retain hashes and commands.','']
for name in evidence:lines.append(f'- [{name}]({name})')
base=OUT/'gpu-independent-dispatch-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n');base.with_suffix('.json').write_text(json.dumps(dict(production_change=False,accepted=False,qualification_cases=117,readonly_cases=234,studies=studies,diagnostics=diag,evidence_sha256=evidence),indent=2)+'\n');print(base.with_suffix('.md'))
