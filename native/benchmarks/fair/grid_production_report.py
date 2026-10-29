"""Audit the promoted aligned-grid implementation, including unfavorable evidence."""
import collections
import hashlib
import json
import statistics as st
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'benchmarks/results'
evidence={}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):
    evidence[str(p.relative_to(ROOT))]=sha(p)
    return json.loads(p.read_text())
def audit(d):
    for field in ('source_sha256','artifact_sha256'):
        for name,digest in d[field].items():
            path=ROOT/name
            if path.exists() and sha(path)==digest:continue
            assert field=='source_sha256' and not Path(name).is_absolute(),name
            blob=subprocess.check_output(['git','show',d['revision']+':'+name],cwd=ROOT)
            assert hashlib.sha256(blob).hexdigest()==digest,name

def timing(name,cells,qpath,broad=False):
    path=OUT/f'gpu-grid-{name}-2026-10-29.json';d=read(path);audit(d)
    assert d.get('verification_sha256',d.get('qualification_sha256'))==sha(qpath)
    assert d['protocol']['blocks']==7 and len(d['observations'])==cells*28
    observations=collections.defaultdict(list);unique=set()
    for r in d['observations']:
        c=r['cell'];g=tuple(c[1:]);key=(g,r['process'],r['slot'])
        assert key not in unique;unique.add(key)
        expected=c[1]*c[3] if broad else c[1]*c[2]*4
        assert [s['round'] for s in r['samples']]==[-2,-1,0,1,2]
        assert all(s['checksum']==expected*r['iterations'] for s in r['samples'])
        if not broad:
            assert all(s['gpu_ns']==s['encoding_ns']==s['dispatches']==0 for s in r['samples'])
            assert all(s['submissions']==r['iterations'] for s in r['samples'])
        observations[(g,r['process'],c[0])].append(st.median(s['ns']/r['iterations'] for s in r['samples'] if s['round']>=0))
    groups=collections.defaultdict(list)
    for r in d['pairs']:
        g=tuple(r['group']);block=r['block']
        for version,field in (('baseline','baseline_ns'),('current','current_ns')):
            vals=observations[(g,block,version)];assert len(vals)==2
            assert abs(st.mean(vals)/r[field]-1)<1e-12
        assert abs(r['time_reduction']-(1-r['current_ns']/r['baseline_ns']))<1e-12
        groups[g].append(r)
    assert len(groups)==cells and all({r['block'] for r in rs}==set(range(7)) for rs in groups.values())
    load=read(path.with_name(path.stem+'-load.json'));values=[s['external_cpu_percent'] for s in load['samples']]
    return dict(protocol=d['protocol'],cells=[dict(shape=g,baseline_us=st.median(r['baseline_ns'] for r in rs)/1000,current_us=st.median(r['current_ns'] for r in rs)/1000,time_reduction_percent=st.median(r['time_reduction']*100 for r in rs),winning_blocks=sum(r['time_reduction']>0 for r in rs),blocks=[r['time_reduction']*100 for r in rs]) for g,rs in sorted(groups.items())],load=dict(context=load['context'],median=st.median(values),maximum=max(values)))

qpath=OUT/'gpu-grid-production-verification-2026-10-29.json';q=read(qpath);audit(q)
assert len(q['checks'])==117 and all(c['bit_identical'] for c in q['checks'])
bpath=ROOT/'benchmarks/.build/gpu-grid-broad-2026-10-29/qualification.json';bq=read(bpath);audit(bq)
assert len(bq['checks'])==78 and all(c['passed'] for c in bq['checks'])
manifest=read(ROOT/'benchmarks/.build/gpu-grid-production-2026-10-29/manifest.json')
for field in ('signature','artifacts'):
    for name,h in manifest[field].items():assert sha(ROOT/name)==h,name
assert len(manifest['matched_kernel_bodies'])==4
xml=OUT/'gpu-grid-production-tests-2026-10-29.xml';log=OUT/'gpu-grid-production-tests-2026-10-29.log'
for file in (xml,log):evidence[str(file.relative_to(ROOT))]=sha(file)
suite=ET.parse(xml).getroot();assert suite.attrib['failures']=='0' and suite.attrib['tests']=='4'
tests=[dict(name=t.attrib['name'],seconds=float(t.attrib['time'])) for t in suite.findall('testcase')]
assert len(tests)==4 and all(t.attrib['status']=='run' for t in suite.findall('testcase'))
for mode in ('optimized','debug','sanitized','thread-sanitized'):assert mode+':' in log.read_text()
whole={n:timing('production-'+n,12,qpath) for n in ('frozen','repeat','settled-aa','settled-confirm')}
broad={n:timing('broad-'+n,5,bpath,True) for n in ('paired','repeat')}
startup_path=OUT/'gpu-grid-production-startup-2026-10-29.json';startup=read(startup_path);audit(startup)
assert len(startup['records'])==56 and len(startup['pairs'])==14
assert startup['verification_sha256']==sha(qpath)
startup_summary=[]
for m in (16,64):
    rows=[r for r in startup['pairs'] if r['m']==m];assert len(rows)==7
    for r in rows:
        for v,field in (('baseline','baseline_ms'),('current','current_ms')):
            observations=[s['process_ms'] for s in startup['records'] if s['m']==m and s['block']==r['block'] and s['version']==v]
            assert len(observations)==2 and st.median(observations)==r[field]
    startup_summary.append(dict(m=m,baseline_ms=st.median(r['baseline_ms'] for r in rows),current_ms=st.median(r['current_ms'] for r in rows),time_reduction_percent=st.median(r['time_reduction_percent'] for r in rows)))
read(startup_path.with_name(startup_path.stem+'-load.json'))
qualification=OUT/'gpu-grid-production-qualification-2026-10-29.json'
qualification.write_text(json.dumps(dict(tests=tests,production_manifest=manifest,broad_qualification=bq,whole_output_cases=117,broad_checks=78),indent=2)+'\n')
evidence[str(qualification.relative_to(ROOT))]=sha(qualification)
evidence[str(Path(__file__).resolve().relative_to(ROOT))]=sha(Path(__file__))
result=dict(production=True,tests=tests,whole_model=whole,broad=broad,startup=startup_summary,evidence_sha256=evidence)
(OUT/'gpu-grid-production-2026-10-29.json').write_text(json.dumps(result,indent=2)+'\n')
lines=['# Production aligned-grid Metal matrices','',
'Apple M2 Max, 29 October 2026. Follow-up to the [general 2D experiments](gpu-grid-dispatch-2026-10-29.md). Whole-model and startup timings use the actual compiler/runtime build; the broader isolated screen and earlier copied-runtime studies are identified separately.','',
'Promoted the aligned variant: final resident transformer time falls 7.75% at batch16 and 7.08% at batch64 (width512). Broader isolated reductions reproduce at 12.82–27.13%. General bounded 2D dispatch is not promoted. Input-upload gains are more variable, all-weight gains are near the timing floor, and warm-cache process startup costs about 0.4–0.5 ms more. Cold driver-cache preparation remains unmeasured.','',
'## Change and contract','',
'The existing M2 Max register16 selection now uses a two-dimensional grid for complete 16×16 output tiles. Removing tile-coordinate division/modulo and redundant boundary predicates preserves every multiply/add, reduction root, barrier and fused epilogue. Ragged outputs retain the original 1D entry. Grouped/skinny choices and other devices keep their previous policy. There is no shape whitelist or function-constant cache. One optional pipeline per eligible plain/fused entry is prepared at module load; no compilation or cache growth occurs during a task. Older bundles omit the new entries and retain their prior behavior.','',
'No language, arithmetic, cancellation, fault-precedence, scheduling or invocation-ownership contract changes. The additional pipelines have explicit preparation-allocation checkpoints and bounded module lifetime. The register study measured 100→94 allocated registers, no spills and a 29.167→35.417% theoretical occupancy ceiling. Achieved occupancy did not increase; this does not prove occupancy caused the speedup.','',
'## Correctness and fault qualification','',
'Four focused CTest groups pass: epilogue code generation, arithmetic policy, Metal pipeline and prepared programs (about 546 seconds total). Runtime checks cover optimized, debug, address/undefined-sanitized and thread-sanitized modes, old/mixed/legacy bundles, allocation-failure sweeps, held cancellation/deadline/device failures, exact outputs, guards and cleanup. This is targeted qualification, not a rerun of the entire compiler suite.','',
'The actual emitted plain kernel and three fused bodies byte-match the qualified transformation. 117 complete-transformer cases match the frozen baseline bit-for-bit and an independent oracle across thirteen shapes, three policies and three upload modes. Another 78 isolated matrix checks cover broader shapes, three patterns and both versions: all GPU output bits/guards are compared; the independent CPU tree checks every cell up to 32,768 output cells and deterministic dispersed cells plus corners above that. Exceptional values establish compatibility with the original GPU kernel, not new independent IEEE conformance.','',
'## Complete transformer timings','',
'Positive values mean less elapsed time; parentheses show winning paired blocks out of seven. Each block uses adjacent ABBA/BAAB fresh processes, identical batch counts, two warmups and three measured rounds. Frozen/repeat target 50 ms with a 512-call cap. The additional identical-build AA and confirmation target 200 ms with a 2,048-call cap after substantial variation appeared on unchanged fallback paths. All samples, including the initial 24% fallback regression, remain retained. AA uses the same current executable and bundle under both labels.','',
'Timed calls check output shape, resource cleanup and submission count, not a numerical checksum. Full numerical checks are separate. All use the existing closed proof; ordinary cancellable controls are in the preceding experiment. Preparation and compilation are excluded from this table.','',
'| M × width / upload | Frozen | Repeat | Long-batch AA | Long-batch confirmation |','|---|---:|---:|---:|---:|']
for i,c in enumerate(whole['frozen']['cells']):
    m,d,mode=c['shape'];label=('resident','input','all weights')[mode]
    lines.append(f'| {m}×{d} / {label} | '+' | '.join(f"{whole[n]['cells'][i]['time_reduction_percent']:+.2f}% ({whole[n]['cells'][i]['winning_blocks']}/7)" for n in whole)+' |')
lines+=['','The batch1/width512 and batch64/width256 cells retain previous kernel selection. Upload timings include memory movement and must not be substituted for resident results. This study does not refresh CPU/vendor-library comparisons or establish general LLM parity.','',
'## Broader isolated matrix timings','',
'Seven paired blocks and an independent repeat, 30 ms target capped at 256 calls. Every output/guard is checked outside timing. These are isolated kernel measurements, not complete-program gains.','',
'| M×K×N | Paired reduction | Repeat reduction |','|---|---:|---:|']
for i,c in enumerate(broad['paired']['cells']):
    lines.append('| '+'×'.join(map(str,c['shape']))+' | '+' | '.join(f"{broad[n]['cells'][i]['time_reduction_percent']:+.2f}% ({broad[n]['cells'][i]['winning_blocks']}/7)" for n in broad)+' |')
lines+=['','## Startup cost','',
'Fresh-process wall time includes loader, module/pipeline preparation, inputs, three invocations and teardown. Seven ABBA/BAAB blocks; OS/Metal caches were already warm. It does not isolate PSO compilation or qualify genuinely cold driver-cache behavior. Added preparation cost remains a tradeoff.','',
'| Batch / width512 | Original process ms | Current process ms | Paired time reduction |','|---|---:|---:|---:|']
for r in startup_summary:lines.append(f"| {r['m']} | {r['baseline_ms']:.2f} | {r['current_ms']:.2f} | {r['time_reduction_percent']:+.2f}% |")
lines+=['','## Load and limits','',
'The user confirmed competing video/photo/GPU apps were closed before these production runs. Tests, builds, captures and acceptance ran sequentially. Five-second process samples exclude the benchmark tree; CPU load does not establish idle GPU utilization. WindowServer and system/UI activity persisted. No samples were discarded, and the short-batch regressions are retained.','',
'| Study | External CPU median | Maximum |','|---|---:|---:|']
for n,r in [*whole.items(),*[(f'broad-{n}',r) for n,r in broad.items()]]:lines.append(f"| {n} | {r['load']['median']:.1f}% | {r['load']['maximum']:.1f}% |")
lines+=['','## Reproduce','',
'Prepare a fresh directory with `grid_production_prepare.py --build DIR`, then run `grid_production_verify.py --build DIR --output FILE`. Use `grid_production_measure.py --verification FILE --build DIR --study frozen|repeat --output FILE`, then `grid_settled_measure.py` with studies `aa|confirm`. `grid_broad_screen.py` supports separate prepare/measure phases; `grid_startup_measure.py` records warm-cache process costs. All require immutable artifacts and fresh output paths. Run `python3 native/benchmarks/fair/grid_production_report.py` after every study completes to audit raw observations, pairs, hashes and qualification. Large generated binaries/captures stay ignored.','',
'- [Audited results and hashes](gpu-grid-production-2026-10-29.json)',
'- [Qualification and emitted-body matches](gpu-grid-production-qualification-2026-10-29.json)',
'- [Full transformer verification](gpu-grid-production-verification-2026-10-29.json)',
'- [CTest log](gpu-grid-production-tests-2026-10-29.log)']
(OUT/'gpu-grid-production-2026-10-29.md').write_text('\n'.join(lines)+'\n')
print('Audited 195 additional correctness executions, four CTest groups, 406 execution pairs and 14 startup pairs.')
