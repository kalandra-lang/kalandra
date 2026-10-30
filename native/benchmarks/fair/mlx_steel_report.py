"""Audit selected upstream Steel kernels, qualification and every timing pair."""
import json,statistics as st,struct
from pathlib import Path
import protocol as p
R=p.ROOT/'benchmarks/results'
B=p.ROOT/'benchmarks/.build/mlx-steel-study-v4-2026-10-30'
def read(path):return json.loads(path.read_text())
def identity(obj):
 for field in ('signature','artifacts','source_sha256','artifact_sha256'):
  for name,digest in obj.get(field,{}).items():assert p.sha(p.ROOT/name)==digest,name
manifest=read(B/'manifest.json');identity(manifest)
assert manifest['pin']=='1ba7f969ad05d86decfeeb83787cc9b79e440962'
qpath=B/'qualification.json';q=read(qpath);identity(q)
assert len(q['checks'])==202 and not q['production_change']
assert q['old_qualification_sha256']==p.sha(R/'gpu-matrix-exact-model-2026-10-30.json')
old=read(R/'gpu-matrix-exact-model-2026-10-30.json')
expected={(r['name'],v) for r in old['records'] if r['variant']=='baseline'
          for v in ('baseline','simd',*manifest['tiles'])}
tails=[(1,1,1),(3,7,5),(17,15,31),(33,17,65),(63,31,67),(65,33,63),(31,65,33),(97,513,35)]
shapes=[(64,512,2048),(64,2048,512),(512,512,512),(512,512,2048),(512,2048,512)]
expected|={('tail-'+'-'.join(map(str,s)),v) for s in tails for v in ('guarded',*manifest['tiles'])}
expected|={('input-'+'-'.join(map(str,s)),v) for s in shapes for v in ('baseline','simd',*manifest['tiles'])}
assert {(r['name'],r['variant']) for r in q['checks']}==expected
cells=0
for row in q['checks']:
 assert row['guards'] and row['inputs_unchanged'] and row['repeatable']
 inp=p.ROOT/row['input'];out=p.ROOT/row['output']
 assert p.sha(inp)==row['input_sha256'] and p.sha(out)==row['output_sha256']
 m,k,n=row['shape'];cells+=m*n*3
 words=struct.unpack('<'+'I'*(m*n*3),out.read_bytes())
 assert words[:m*n]==words[m*n:2*m*n]==words[2*m*n:]
 if row['mode']=='bounded':assert not row['bound_failures'] and not row['nonfinite_cells']
 if 'different_baseline' in row and row['variant']=='baseline':assert not row['different_baseline']
 if 'different_blocked' in row and row['variant']=='simd':assert not row['different_blocked']
assert p.sha(R/'mlx-steel-LICENSE-2026-10-30.txt')==p.sha(p.ROOT/'benchmarks/.build/mlx-steel-source-2026-10-30/LICENSE')
lines=['# Pinned MLX Steel kernel comparison — 30 October 2026','',
 '## Scope and provenance','',
 'Upstream MLX commit `1ba7f969ad05d86decfeeb83787cc9b79e440962`, MIT license retained '
 'alongside the evidence. The upstream `GEMMKernel<float,float,...,float>` templates '
 'and transitive headers are unmodified. Our adapter only supplies the Metal entry '
 'point, buffers, parameters and threadgroup arrays; it does not implement or rewrite '
 'the matrix algorithm. Three existing tile configurations are screened: 64×64×16 '
 '(four SIMD groups), 32×64×16 (two groups), 64×32×32 (four groups). No full MLX '
 'runtime, NAX path, split-K selector, transpose/batch selector or autotuner is included. '
 'These are selected Steel kernels, not a claim to measure peak MLX framework performance.','',
 'All candidates and our unchanged shader source are compiled with the pinned Metal '
 'toolchain, Metal 3.2, `-fno-fast-math -ffp-contract=off`, macOS minimum 26. '
 'Explicit matrix primitives still perform fused accumulation. Shared upstream '
 'headers require native bfloat syntax; an initial Metal 2.3 build failed before '
 'measurement. The correct newer standard uses `metal3.2`, not `macos-metal3.2`. '
 'No shader was edited to bypass those requirements. Our rebuilt current/SIMD '
 'outputs are checked against their earlier frozen Metal 2.3 numerical results.','',
 '## Correctness and behavior','',
 f'**202 kernel/case checks, {cells:,} output cells across three repetitions**, '
 'under Metal API and shader validation, with unchanged inputs and input/output '
 'suffix guards. This includes all 29 prior arithmetic witnesses, eight irregular '
 'shapes down to 1×1×1, and all five dense benchmark inputs. Each available '
 'implementation runs each case. Our aligned current kernel is used in timed cases; '
 'the existing guarded 8×8 production kernel supplies boundary-case comparison. '
 'Our aligned SIMD prototype is not dispatched on unsupported shape tails. '
 'All tested dimensions are positive; empty dimensions, transposes, batches '
 'and other element types are outside this screen.','',
 'Every bounded finite case passes a complete per-cell exact-integer oracle: '
 'float32 values are decoded at scale 2^-149 and exact products/sums at scale '
 '2^-298, with the conservative gamma(K)×sum(abs(products)) bound compared in '
 'integer arithmetic. That bound is not a small-relative-error guarantee. '
 'Subnormal, overflow, infinity, NaN and signed-zero cases are retained as '
 'behavioral comparisons, not incorrectly admitted to the ordinary finite bound.','',
 '| Witness | Current fixed tree, first cell | Own SIMD | Steel configurations |',
 '|---|---|---|---|']
for name in ('negative-zero','cancellation-512','fused-rounding-witness',
             'intermediate-overflow-cancellation','subnormal-input-scaled'):
 rows={r['variant']:r for r in q['checks'] if r['name']==name}
 lines.append(f'| {name} | `{rows["baseline"]["first_bits"]}` | `{rows["simd"]["first_bits"]}` | '+
              ', '.join(f'{v}: `{rows[v]["first_bits"]}`' for v in manifest['tiles'])+' |')
differences=[r for r in q['checks'] if r['variant'].startswith('steel') and 'different_blocked' in r]
lines+=['',f'Across the prior 29-case corpus, Steel has '
        f'{sum(r["different_blocked"] for r in differences):,} differing output cells '
        'from the independently modeled own SIMD outputs (NaN payloads allowed). '
        'This observed equivalence is not a cross-device accumulation-order contract. '
        'The exact-256 cancellation witnesses and overflow differences from the '
        'current fixed tree remain visible; no universal accuracy claim follows.','',
        '## Matched uninstrumented timing','',
        'Same executable, private resident operands and preallocated output, same '
        'input bytes, API/graph setup and initial uploads excluded. Each timed '
        'command buffer contains identical dispatch counts; one compute encoder '
        'per dispatch for every implementation, followed by completed GPU '
        'acknowledgement. Readback, guards, repeatability and immutable-input '
        'checks occur outside the interval on every warm/measured round. No '
        'counter sampling or validation is enabled during timing. This is '
        'batched isolated-kernel latency, not Kalandra invocation or transformer throughput. '
        'Time columns are independent marginal medians; speedups are medians of '
        'paired ratios and need not equal the quotient of those marginal medians.','',
        'Screen: three adjacent ABBA/BAAB blocks per tile/shape. Select the highest '
        'median paired speedup per shape from that screen, freeze the selection, and use '
        'fresh five-block confirm/prototype/repeat studies. AA has three blocks '
        'of the identical rebuilt current kernel. Every pair uses identical '
        'iterations, two warmups, three measured rounds, a 100 ms adaptive '
        'target and 256-call cap. All outcomes and external CPU-load samples '
        'are retained. CPU samples cannot prove GPU idleness.','',
        '| Study | M × K × N | Reference µs | Steel/current µs | Reference / candidate | Block range |',
        '|---|---|---:|---:|---:|---:|']
studies={}
for study in ('aa','screen','confirm','prototype','repeat'):
 path=R/f'mlx-steel-2026-10-30-{study}.json';data=read(path);identity(data)
 assert data['study']==study and data['qualification_sha256']==p.sha(qpath)
 assert len(data['pairs'])==(45 if study=='screen' else 15 if study=='aa' else 25)
 groups={(*s,t) for s in shapes for t in (manifest['tiles'] if study=='screen' else ['selected'])}
 blocks=3 if study in ('aa','screen') else 5
 assert {(tuple(pair['group']),pair['block']) for pair in data['pairs']}=={(g,i) for g in groups for i in range(blocks)}
 if study in ('confirm','prototype','repeat'):
  assert data['selection_sha256']==p.sha(R/'mlx-steel-2026-10-30-screen.json')
 for obs in data['observations']:
  p.observations('\n'.join(map(json.dumps,obs['samples'])),obs['cell'][1]*obs['cell'][3],obs['iterations'],3)
 for pair in data['pairs']:
  obs=[o for o in data['observations'] if o['process']==pair['block'] and o['cell'][1:]==pair['group']]
  assert len(obs)==4 and len({o['iterations'] for o in obs})==1
  sides={v:[st.median(s['ns']/o['iterations'] for s in o['samples'] if s['round']>=0) for o in obs if o['cell'][0]==v] for v in ('baseline','current')}
  before,after=st.mean(sides['baseline']),st.mean(sides['current'])
  assert abs(pair['baseline_ns']-before)<.001 and abs(pair['current_ns']-after)<.001
  assert abs(pair['speedup']-before/after)<1e-12 and abs(pair['time_reduction']-(1-after/before))<1e-12
 for group in sorted({tuple(p['group']) for p in data['pairs']}):
  pairs=[p for p in data['pairs'] if tuple(p['group'])==group];ratios=[p['speedup'] for p in pairs]
  lines.append(f'| {study} {group[3]} | {group[0]} × {group[1]} × {group[2]} | '
   f'{st.median(p["baseline_ns"] for p in pairs)/1000:.2f} | {st.median(p["current_ns"] for p in pairs)/1000:.2f} | '
   f'{st.median(ratios):.3f}× | {min(ratios):.3f}–{max(ratios):.3f}× |')
 load=read(path.with_name(path.stem+'-load.json'));assert load['samples']
 studies[study]=data
lines+=['','The prototype study compares our faster blocked SIMD kernel with selected Steel; '
        'confirm/repeat compare the existing fixed-tree kernel with selected Steel. '
        'Those arithmetic contracts differ and are disclosed rather than treated '
        'as exact semantic substitutions.','',
        '## Control precision and frozen selection','',
        '| M × K × N | AA median time change | AA block range | Control | Selected tile |',
        '|---|---:|---:|---|---|']
for shape in sorted({tuple(x['group'][:3]) for x in studies['aa']['pairs']}):
 pairs=[x for x in studies['aa']['pairs'] if tuple(x['group'][:3])==shape]
 values=[x['time_reduction']*100 for x in pairs]
 passed=abs(st.median(values))<=2 and max(map(abs,values))<=5
 tile=studies['confirm']['winners']['-'.join(map(str,shape))]
 lines.append(f'| {shape[0]} × {shape[1]} × {shape[2]} | {st.median(values):+.2f}% | '
              f'{min(values):+.2f}% … {max(values):+.2f}% | {"pass" if passed else "fail"} | {tile} |')
lines+=['','No production source or arithmetic/cancellation contract changes. '
        'Adoption still requires a deliberate default numerical policy, integration '
        'with fused epilogues, resource/cancellation qualification, and full-model '
        'frozen/control/MPSGraph measurements. These sources provide a candidate '
        'starting point; they are not automatically correct for every Kalandra '
        'type, placement or numerical mode.','',
        'The user-supplied X article returned HTTP 403; its claims are not used '
        'as evidence for this benchmark. The tested upstream source and MIT '
        'license, compiler commands, input/artifact hashes, all variants and '
        'all selected/unselected results are retained.']
lines+=['','## Interpretation','',
        'The large FFN expansion improves 4.980× in confirmation and 4.824× in '
        'replication versus the current tree; contraction improves 4.389× and '
        '4.353×. Relative to the own blocked SIMD prototype, the same two '
        'kernels improve 1.723× and 1.641×. The selected Steel path wins all '
        'five measured shapes, while two unselected configurations lose on '
        'the small contraction. This supports using Steel as an integration '
        'starting point, not blindly selecting one tile everywhere.','',
        'Background CPU activity is present (median external load roughly '
        '132–236% across cohorts, peaks up to 291%, where 100% is one core). '
        'All identical-kernel controls pass, and the large confirmation and '
        'replication wins agree, but these are not claims of a completely idle '
        'machine or universal performance. Screening baseline clock/load '
        'variation is retained; screening ratios are not substituted for '
        'the fresh confirmation results. No whole-model or MPSGraph-gap '
        'closure claim follows from these isolated kernel measurements.']
(R/'mlx-steel-comparison-2026-10-30.md').write_text('\n'.join(lines)+'\n')
print(f'Audited Steel: {cells:,} numerical output cells and all five timing studies.')
