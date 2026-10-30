"""Final-runtime numerical-library comparison and separate encoder diagnosis.

MPSGraph is a benchmark-only reference. Both paths compute the same authored
block and meet independent numerical bounds; operation order is not identical.
No profiling/counters overlap uninstrumented library comparisons.
"""
import argparse,gzip,json,os,statistics as st,struct,subprocess,sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure','profile'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('aa','library','repeat'),default='library')
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
f=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29';accepted=p.ROOT/'benchmarks/.build/gpu-prepared-output-fastmiss-frozen-2026-10-29'
counter=b/'counters/probe';manifest=b/'manifest.json';fair=Path(__file__).parent/'fair'
sources=[Path(__file__),Path(p.__file__),Path(ref.__file__),fair/'TransformerMps.mm',fair/'TransformerBlock.h',fair/'ResidualFfn.h',p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/metal/Metal.h',b/'counters/manifest.json']
sig={p.key(x.resolve()):p.sha(x) for x in sources}
if a.phase=='prepare':
 assert b.exists() and not manifest.exists()
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-ffp-contract=off','-mmacosx-version-min=26.0',fair/'TransformerMps.mm','-framework','Foundation','-framework','Metal','-framework','MetalPerformanceShaders','-framework','MetalPerformanceShadersGraph','-o',b/'mps'])
 files=[accepted/'gpu',accepted/'Driver.cpp',counter,b/'mps',f/'TransformerProgram.h',*(f/'bundle').glob('*.metallib')]
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},commands=p.COMMANDS),indent=2)+'\n');a.output.write_text(manifest.read_text());sys.exit(0)
saved=json.loads(manifest.read_text());assert saved['signature']==sig
for name,digest in saved['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
def command(kind,m,d,count,rounds,dump=0,pattern=0):
 if kind=='mps':return [b/'mps',m,d,count,rounds,0,dump,pattern,1]
 return [counter if kind=='counter' else accepted/'gpu',f/'bundle',m,d,count,rounds,0,dump,pattern,2,1]
decode=lambda text:json.loads(text,parse_int=lambda n:-0.0 if n=='-0' else int(n))
bits=lambda row:struct.pack('<'+'f'*len(row['cells']),*row['cells'])
if a.phase=='verify':
 checks=[]
 for m,d in ((3,33),(1,512),(64,512),(512,512)):
  for pattern in (0,1):
   results={}
   for kind in ('gpu','mps','counter'):
    cmd=list(map(str,command(kind,m,d,1,1,1,pattern)));p.COMMANDS.append(cmd)
    result=subprocess.run(cmd,cwd=p.ROOT,capture_output=True,text=True,timeout=180,env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'))
    assert result.returncode==0,(kind,result.stderr[-3000:])
    row=decode(result.stdout);metrics,_=ref.assess(m,d,pattern,row);results[kind]=row
    checks.append(dict(m=m,d=d,pattern=pattern,backend=kind,metrics=metrics))
   assert bits(results['counter'])==bits(results['gpu'])
  print(m,d,'all full outputs and counter identity passed',flush=True)
 data=dict(contract=__doc__,checks=checks,counter_bit_identical=True)
else:
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==24 and qualified['counter_bit_identical']
 if a.phase=='measure':
  def invoke(cell,count,rounds):
   version,m,d=cell;kind='gpu' if a.study=='aa' or version=='baseline' else 'mps'
   rows=[json.loads(s) for s in p.run(command(kind,m,d,count,rounds),timeout=900).splitlines()]
   return p.observations('\n'.join(json.dumps(r) for r in rows if 'ns' in r),m*d*4,count,rounds)
  cells=[[version,m,512] for m in (1,64,512) for version in ('baseline','current')]
  data=p.measure_paired(cells,invoke,lambda cell:cell[1]*cell[2]*4,blocks=3,rounds=3,target_ms=500,adaptive=True,max_iterations=8192)
  data.update(contract=__doc__,study=a.study,baseline='current Kalandra',current='same Kalandra' if a.study=='aa' else 'MPSGraph',qualification_sha256=p.sha(q))
 else:
  records=[];rawdir=b/'raw';rawdir.mkdir(exist_ok=True)
  for process in range(3):
   for m in (64,512):
    cmd=list(map(str,command('counter',m,512,64,3)));p.COMMANDS.append(cmd)
    result=subprocess.run(cmd,cwd=p.ROOT,capture_output=True,text=True,timeout=180);assert result.returncode==0,result.stderr[-3000:]
    raw=rawdir/f'{process}-{m}.json.gz'
    with gzip.open(raw,'wt') as out:json.dump(dict(command=cmd,stdout=result.stdout,stderr=result.stderr),out)
    samples=[json.loads(s) for s in result.stderr.splitlines() if s.startswith('{"dispatch_counter_call":')]
    assert len(samples)==320 and all(len(x['passes'])==12 for x in samples)
    timed=[json.loads(s) for s in result.stdout.splitlines()];p.observations(result.stdout,m*512*4,64,3)
    passes=[]
    for i in range(12):
     pcs={s['passes'][i]['pc'] for s in samples};assert len(pcs)==1
     vals=[s['passes'][i]['ns'] for s in samples[128:]]
     passes.append(dict(pc=next(iter(pcs)),median_ns=st.median(vals),minimum_ns=min(vals),maximum_ns=max(vals)))
    records.append(dict(process=process,m=m,d=512,count=64,rounds=3,passes=passes,command_gpu_ns=st.median(s['command_gpu_ns'] for s in samples[128:]),observations=timed,raw=p.key(raw),raw_sha256=p.sha(raw)))
    print(m,'process',process,'encoder intervals recorded',flush=True)
  data=dict(contract=__doc__,acceptance=False,qualification_sha256=p.sha(q),records=records,note='Encoder sampling changes overhead; intervals include stalls/scheduling and are not exclusive compute times. Never use these wall times as library comparisons.')
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],p.ROOT/'build/native/native/kalandra')
