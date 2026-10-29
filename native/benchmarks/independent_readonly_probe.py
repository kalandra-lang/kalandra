"""Copied-bundle experiment: declare register16 inputs read-only, without changing arithmetic."""
import argparse,json,os,shutil,struct,subprocess,sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS']='1';os.environ['VECLIB_MAXIMUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
import transformer_reference as ref
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('phase',choices=('prepare','verify','measure'));a=parser.parse_args()
b=p.ROOT/'benchmarks/.build/gpu-independent-readonly-2026-10-29';runtime=p.ROOT/'benchmarks/.build/gpu-independent-dispatch-v2-2026-10-29'
original=p.ROOT/'benchmarks/.build/gpu-attention-2026-10-29/acceptance/bundle';out=p.ROOT/'benchmarks/results'
qfile=out/'gpu-independent-readonly-verification-2026-10-29.json';timing=out/'gpu-independent-readonly-profile-2026-10-29.json'
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=False)
 text=(original/'kernel0-stage1.metal').read_text();start=text.index('kernel void kalandra_matmul_hardware_register16(');end=text.index(') {',start)
 signature=text[start:end]
 for typ,name in [('KalandraHardwareMatrix','a'),('KalandraHardwareMatrix','b'),('KalandraHardwareShape','parameters')]:
  old=f'device {typ}& {name} [[';assert signature.count(old)==1;signature=signature.replace(old,f'const device {typ}& {name} [[')
 current=text[:start]+signature+text[end:]
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 for mode,source in [('baseline',text),('current',current)]:
  dest=b/mode;shutil.copytree(original,dest);(dest/'kernel0-stage1.metal').write_text(source)
  p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',dest/'kernel0-stage1.metal','-o',dest/'kernel0-stage1.air'])
  p.run([*metal,dest/'kernel0-stage1.air','-o',dest/'kernel0-stage1.metallib'])
 sources=[Path(__file__),original/'kernel0-stage1.metal',p.ROOT/'native/metal-proof/toolchain.json']
 artifacts=[runtime/'gpu',runtime/'profile',*[f for version in ('baseline','current') for f in (b/version).glob('*.metallib')]]
 (b/'manifest.json').write_text(json.dumps(dict(source_sha256={p.key(f.resolve()):p.sha(f) for f in sources},artifact_sha256={p.key(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
else:
 manifest=b/'manifest.json';saved=json.loads(manifest.read_text())
 for field in ('source_sha256','artifact_sha256'):
  for name,digest in saved[field].items():assert p.sha(p.ROOT/name)==digest,name
 if a.phase=='verify':
  assert not qfile.exists();checks=[]
  def invoke(bundle,m,d,pattern,policy,mode):
   command=list(map(str,[runtime/'gpu',bundle,m,d,1,1,mode,1,pattern,policy,1]));p.COMMANDS.append(command)
   result=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,KALANDRA_QKV_CONCURRENT='1',KALANDRA_QKV_TRACE='1',MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'),timeout=180)
   assert result.returncode==0,(command,result.stderr)
   return json.loads(result.stdout),[line for line in result.stderr.splitlines() if line.startswith('QKV_GROUP ')]
  for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,0) for d in (64,256,512) for m in (1,16,64)]:
   old,_=invoke(original,m,d,pattern,2,0);expected=struct.pack('<'+'f'*len(old['cells']),*old['cells'])
   for version in ('baseline','current'):
    for policy in (0,1,2):
     for mode in (0,1,2):
      row,groups=invoke(b/version,m,d,pattern,policy,mode)
      assert struct.pack('<'+'f'*len(row['cells']),*row['cells'])==expected
      assert groups==(['QKV_GROUP 11 15'] if policy==2 and m>=16 and d>=512 else [])
      metrics,_=ref.assess(m,d,pattern,row);checks.append(dict(version=version,m=m,d=d,pattern=pattern,policy=policy,mode=mode,bit_identical=True,oracle=metrics))
   print(m,d,'passed',flush=True)
  data=dict(acceptance=False,checks=checks,contract=__doc__)
  target=qfile
 else:
  assert not timing.exists();q=json.loads(qfile.read_text());assert len(q['checks'])==234
  def invoke(cell,count,rounds):
   version,m,d,concurrent=cell
   rows=p.observations(p.run([runtime/'profile',b/version,m,d,count,rounds,0,0,0,2,1],env=dict(os.environ,KALANDRA_QKV_CONCURRENT=str(concurrent))),m*d*4,count,rounds)
   assert all(r['gpu_ns']>0 for r in rows);return rows
  data=p.measure_paired([(version,m,512,c) for m in (16,64) for c in (0,1) for version in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=5,target_ms=50,adaptive=True,max_iterations=256)
  data.update(acceptance=False,qualification_sha256=p.sha(qfile),contract='Options.profile diagnostic. Same compilation and runtime; only register16 input/shape const qualifiers differ. Values separately qualified; timings check resource cleanup/shape, not numerical checksum.')
  target=timing
 p.evidence(target,data,[Path(__file__),manifest,*[p.ROOT/f for f in saved['source_sha256']]],[*[p.ROOT/f for f in saved['artifact_sha256']]],p.ROOT/'build/native/native/kalandra')
