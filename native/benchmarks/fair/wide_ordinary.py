"""Signed/wide runtime-input corpus; unchanged source-level algorithms and ABIs."""
import argparse,functools,json,math,struct,sys
from decimal import Decimal,localcontext
from pathlib import Path
import protocol as p
PROGRAMS=('affine','polynomial','euclid','euclid_mixed','array32','array128')
SEEDS={
 'wide-positive':(2**40+17,)*8,
 'wide-negative':(-2**40-17,)*8,
 'rotating':(17,-17,65536,-65536,2**32+17,-2**32-17,2**40+17,-2**40-17),
}
COUNTS=(1,16,4096)
MASK=(1<<64)-1

def remainder(a,b):
 assert b
 return a-((abs(a)//abs(b))*(-1 if (a<0)!=(b<0) else 1))*b

def integer(x):
 assert -(1<<63)<=x<(1<<63),('oracle overflow',x)
 return x

@functools.cache
def oracle(name,seed,n):
 if name.startswith('array'):
  values=[integer(seed+i) for i in range(int(name[5:]))]
  for _ in range(n):values=[remainder(integer(integer(x*13)+7),1000003) for x in values]
  return float(integer(sum(values)))
 if name in ('euclid','euclid_mixed'):
  x=seed
  for _ in range(n):
   a=integer(integer(x*13)+7);b=integer(integer(seed*7)+11)
   while b:a,b=b,remainder(a,b)
   if name=='euclid_mixed':x=remainder(integer(integer(integer(x*48271)+a)+seed),2147483647)
   else:x=integer(a+seed)
  return float(x)
 with localcontext() as ctx:
  ctx.prec=200;x=seed*(.25 if name=='affine' else .015625);offset=seed*(.0625 if name=='affine' else .00390625);scale=.5 if name=='affine' else 1.015625
  for _ in range(n):x=float(Decimal.from_float(float(Decimal.from_float(x)*Decimal.from_float(scale)))+Decimal.from_float(offset))
  assert math.isfinite(x)
  return x

def bits(x):return struct.unpack('<Q',struct.pack('<d',x))[0]
def reference(cell):
 _,name,regime,n=cell
 return [oracle(name,s,n) for s in SEEDS[regime]]
def invoke(directory,cell,count,rounds):
 lang,name,regime,n=cell;expected=reference(cell);values=[bits(x) for x in expected];cycles,tail=divmod(count,8);checksum=(sum(values)*cycles+sum(values[:tail]))&MASK
 output=p.run([directory/name/('wide-'+lang),n,count,rounds,*SEEDS[regime],*map(repr,expected)])
 rows=[json.loads(s) for s in output.splitlines()];assert [r['round'] for r in rows]==list(range(-2,rounds))
 for r in rows:assert math.isfinite(r['ns']) and r['ns']>=0 and r['checksum']==checksum,(cell,count,r,checksum)
 return rows

def artifacts(directory):
 m=json.loads((directory/'wide-manifest.json').read_text())
 for f,h in m['hashes'].items():assert p.sha(f)==h,f
 return m

def main():
 q=argparse.ArgumentParser(description=__doc__);q.add_argument('--compiler',type=Path);q.add_argument('--build-dir',type=Path);q.add_argument('--output',type=Path,required=True);q.add_argument('--build-only',action='store_true');q.add_argument('--reuse-build',action='store_true');q.add_argument('--baseline',type=Path);q.add_argument('--current',type=Path);a=q.parse_args();assert not a.output.exists()
 src=Path(__file__).resolve().parent;sources=[Path(__file__).resolve(),src/'WideDriver.cpp',src/'broad_ordinary.py',src/'BroadScalar.k',src/'BroadMixed.k',src/'BroadReference.cpp',src/'broad_reference.rs',src/'protocol.py',src/'BroadDriver.cpp',p.ROOT/'native/runtime/Runtime.h',p.ROOT/'native/runtime/OrdinaryFault.h']
 if a.baseline:
  dirs={'baseline':a.baseline.resolve(),'current':a.current.resolve()};ms={k:artifacts(v) for k,v in dirs.items()};compiler=Path(ms['current']['compiler'])
  assert ms['baseline']['source_sha256']==ms['current']['source_sha256']
  cells=[[v,name,regime,n] for v in dirs for name in PROGRAMS for regime in SEEDS for n in COUNTS]
  def call(cell,count,rounds):return invoke(dirs[cell[0]],['kalandra',*cell[1:]],count,rounds)
  result=p.measure_paired(cells,call,reference,blocks=9,rounds=3,target_ms=10,adaptive=True,max_iterations=20000000)
  files=[Path(f) for m in ms.values() for f in m['hashes']];result['manifests']=ms
 else:
  directory=a.build_dir.resolve();compiler=a.compiler.resolve();runtime=compiler.parent/'libkalandra-task-core-optimized.a';signature={p.key(f):p.sha(f) for f in sources}
  if not a.reuse_build:
   assert not (directory/'manifest.json').exists(),'Use a fresh build directory'
   p.run([sys.executable,src/'broad_ordinary.py','--compiler',compiler,'--build-dir',directory,'--build-only','--output',directory/'unused.json'])
   for name in PROGRAMS:
    m=json.loads((directory/'manifest.json').read_text());flag={'ordinary-direct-v1':'-DDIRECT_ENTRY','ordinary-pair-v1':'-DPAIR_ENTRY','ordinary-v1':'-DORDINARY_ENTRY','checked-v4':'-DCHECKED_ENTRY'}[m['entry_abi'][name]];folder=directory/name
    for lang,inputs,define in [('kalandra',[folder/'source.ll',runtime],[flag]),('cpp',[folder/'cpp.o'],[]),('rust',[folder/'rust.a'],[])]:
     p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-Wno-override-module','-pthread',*define,src/'WideDriver.cpp',*inputs,'-o',folder/('wide-'+lang)])
   files=[f for f in directory.rglob('*') if f.is_file() and f.name!='wide-manifest.json']+[compiler,runtime]
   saved=dict(compiler=str(compiler),source_sha256=signature,hashes={str(f):p.sha(f) for f in files},commands=p.COMMANDS)
   (directory/'wide-manifest.json').write_text(json.dumps(saved,indent=2)+'\n')
  saved=artifacts(directory);assert saved['source_sha256']==signature;assert saved['compiler']==str(compiler)
  if a.build_only:return
  files=[Path(f) for f in saved['hashes']];cells=[[lang,name,regime,n] for lang in ('kalandra','cpp','rust') for name in PROGRAMS for regime in SEEDS for n in COUNTS]
  verification=[dict(cell=cell,iterations=9,samples=invoke(directory,cell,9,1)) for cell in cells]
  result=p.measure(cells,lambda cell,count,rounds:invoke(directory,cell,count,rounds),reference,target_ms=10,adaptive=True,max_iterations=20000000);result['manifest']=saved;result['verification']=verification
 result.update(execution_contract='ordinary',seeds=SEEDS,contract='Unchanged equivalent source algorithms; signed truncating remainder, exact returned bits and wrapping checksums on every warmup/calibration/timed call. Eight runtime seeds selected cyclically per call, same driver for every language. Strict binary64 multiply then add, no FMA/reassociation; signed overflow excluded by independent oracle, not by changing source. No build/test/profiler overlap; no affinity/frequency/desktop isolation.',source_snapshots={p.key(f):f.read_text() for f in sources})
 p.evidence(a.output,result,sources,files,compiler)
if __name__=='__main__':main()
