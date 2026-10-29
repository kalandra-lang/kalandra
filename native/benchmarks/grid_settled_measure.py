"""Longer-batch production confirmation and identical-build noise control.

AA uses the same current binary and bundle under both labels. Confirm compares
frozen original versus actual production. Complete numerical/fault qualification
is separate; timing validates shape, cleanup and submission counts.
"""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--verification',type=Path,required=True);parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--study',choices=('aa','confirm'),required=True);parser.add_argument('--blocks',type=int,default=7)
a=parser.parse_args();assert not a.output.exists()
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_QKV_TRACE'):assert os.getenv(flag) in (None,'','0'),flag
build=a.build.resolve();manifest=build/'manifest.json';saved=json.loads(manifest.read_text())
verification=a.verification.resolve()
q=json.loads(verification.read_text());assert len(q['checks'])==117 and all(r['bit_identical'] for r in q['checks'])
for item in (saved,):
 for field in ('signature','artifacts'):
  for file,digest in item[field].items():assert p.sha(p.ROOT/file)==digest,file
for field in ('source_sha256','artifact_sha256'):
 for file,digest in q[field].items():assert p.sha(p.ROOT/file)==digest,file
old=p.ROOT/'benchmarks/.build/gpu-closed-entry-2026-10-29/acceptance/gpu'
bundle=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle'
shapes=[(1,512),(64,256),(16,512),(64,512)]
cells=[(version,m,d,mode) for m,d in shapes for mode in (0,1,2) for version in ('baseline','current')]
def invoke(cell,count,rounds):
 version,m,d,mode=cell
 binary=build/'gpu' if version=='current' or a.study=='aa' else old
 policy=2
 selected_bundle=build/'bundle' if version=='current' or a.study=='aa' else bundle
 command=[binary,selected_bundle,m,d,count,rounds,mode,0,0,policy,1]
 env=dict(os.environ)
 rows=p.observations(p.run(command,env=env),m*d*4,count,rounds)
 assert all(r['gpu_ns']==0 and r['encoding_ns']==0 and r['dispatches']==0 for r in rows)
 if policy==2:assert all(r['submissions']==count for r in rows)
 return rows
result=p.measure_paired(cells,invoke,lambda c:c[1]*c[2]*4,blocks=a.blocks,target_ms=200,adaptive=True,max_iterations=2048)
result.update(contract=__doc__,study=a.study,verification_sha256=p.sha(verification),production_change=True,policy='ordinary cancellable' if a.study=='ordinary' else 'closed proof',validation_scope='Separate 117 full-output cases with independent oracle; timed invocations check shape, resource cleanup and command count. No value-derived timing checksum.')
p.evidence(a.output,result,[Path(__file__),manifest,verification,*[p.ROOT/f for f in saved['signature']]],[old,*[p.ROOT/f for f in saved['artifacts']],*bundle.glob('*.metallib')],p.ROOT/'build/native/native/kalandra')
