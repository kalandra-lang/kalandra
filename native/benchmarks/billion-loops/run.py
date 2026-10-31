"""Local, sequential, checked-output benchmark. Never builds during timing."""
import argparse,ast,collections,hashlib,json,os,platform,random,statistics,subprocess,time,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
SOURCE=Path(__file__).resolve().parent
BUILD=ROOT/'build/billion-loops'
COMPILER=ROOT/'build/native/native/kalandra'
CLANG=Path(json.loads((ROOT/'native/toolchain-proof/toolchain.json').read_text())['defaultLlvmRoot'])/'bin/clang++'
RUST=Path.home()/'.rustup/toolchains/stable-aarch64-apple-darwin/bin/rustc'
PROGRAMS=['kalandra','cpp','rust','cpp-checked','rust-checked']
TIMED=PROGRAMS+['cpp-repeat']
def run(argv,timeout=180):
 p=subprocess.run(list(map(str,argv)),cwd=ROOT,text=True,capture_output=True,timeout=timeout)
 assert p.returncode==0,(argv,p.returncode,p.stdout,p.stderr)
 return p.stdout.strip()
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def mathematical_sum(u,count):
 cycles,remaining=divmod(count,u)
 return cycles*u*(u-1)//2+remaining*(remaining-1)//2

def build():
 BUILD.mkdir(parents=True,exist_ok=True)
 for program,source in [('kalandra','benchmark.k'),('kalandra-verify','verify.k')]:
  run([COMPILER,SOURCE/source,'-O2','--emit-llvm',BUILD/(program+'.ll'),'-o',BUILD/program])
 for program,checked in [('cpp',False),('cpp-checked',True)]:
  flags=['-std=c++23','-O3','-mmacosx-version-min=26.0']+(['-DCHECKED'] if checked else [])
  run([CLANG,*flags,SOURCE/'reference.cpp','-o',BUILD/program])
  run([CLANG,*flags,SOURCE/'reference.cpp','-S','-emit-llvm','-o',BUILD/(program+'.ll')])
 for program,checked in [('rust',False),('rust-checked',True)]:
  flags=['-C','opt-level=3']+(['-C','overflow-checks=yes'] if checked else [])
  run([RUST,*flags,SOURCE/'reference.rs','-o',BUILD/program])
  run([RUST,*flags,SOURCE/'reference.rs','--emit=llvm-ir','-o',BUILD/(program+'.ll')])
 for program in PROGRAMS:
  (BUILD/(program+'.asm')).write_text(run([CLANG.parent/'llvm-objdump','--disassemble','--no-show-raw-insn',BUILD/program]))
 shutil.copy2(BUILD/'cpp',BUILD/'cpp-repeat')
 print('Frozen candidate/reference binaries and optimized IR/assembly built')

def verify():
 records=[]
 cases=[(1,1,0),(1,7,100),(2,31,101),(3,17,1000),(7,31,97),(40,10000,100000),(97,19,1001),(2147483647,7,100)]
 for program in PROGRAMS:
  for u,rows,count in cases:
   args=[BUILD/'kalandra-verify','--all','--',str(u),str(rows),str(count)] if program=='kalandra' else [BUILD/program,str(u),str(rows),str(count),'--verify']
   values=ast.literal_eval(run(args))
   r,*array=values
   expected=mathematical_sum(u,count)+r
   assert 0<=r<10000 and len(array)==rows and all(v==expected for v in array),(program,u,rows,count)
   records.append(dict(program=program,u=u,rows=rows,count=count,r=r,expected=expected,elementsVerified=len(array)))
  print(program,'full arrays verified for',len(cases),'input combinations',flush=True)
 (BUILD/'correctness.json').write_text(json.dumps(dict(reference='independent integer arithmetic-series formula',cases=records),indent=2)+'\n')

def load():
 p=subprocess.run(['/bin/ps','-A','-o','pid=,pcpu=,comm='],capture_output=True,text=True,check=True)
 processes=[]
 for line in p.stdout.splitlines():
  fields=line.strip().split(None,2)
  if len(fields)==3:
   try:processes.append(dict(pid=int(fields[0]),cpu=float(fields[1]),name=Path(fields[2]).name))
   except ValueError:pass
 return dict(totalCPU=sum(p['cpu'] for p in processes),top=sorted(processes,key=lambda p:p['cpu'],reverse=True)[:8])

def timing():
 frozen={name:sha(BUILD/name) for name in TIMED}
 assert frozen['cpp']==frozen['cpp-repeat']
 data=dict(sourceSHA256={name:sha(SOURCE/name) for name in ('benchmark.k','verify.k','reference.cpp','reference.rs')},host=platform.platform(),cpu=run(['sysctl','-n','machdep.cpu.brand_string']),
  compiler=run([COMPILER,'--version'])+' + local experimental APIs',clang=run([CLANG,'--version']).splitlines()[0],rust=run([RUST,'--version','-v']),
  git=run(['git','rev-parse','HEAD']),diffSHA256=hashlib.sha256(run(['git','diff']).encode()).hexdigest(),binarySHA256=frozen,
  protocol=dict(unit='end-to-end process elapsed seconds',blocks=7,seed=20261031,u=40,rows=10000,inner=100000,logicalIterations=1000000000,parallelism='sequential CPU',notes=['Runtime inputs; one random draw and array allocation included.','C++/Rust include original zero initialization; generate initializes each output once.','Random distributions are equivalent; RNG implementations differ.','Ordinary C++/Rust overflow checks disabled; extra controls enable checked accumulation.','Valid input domain excludes integer overflow.', 'Output packs actual random r and selected result into one int64 for independent per-run checks.', 'Identical C++ binary repeated as a timing control. Background macOS maintenance is recorded; 400% pre-run stop threshold.']),observations=[],scaling=[])
 output=BUILD/'results.json'
 def trial(program,rows,count,kind,block):
  before=load()
  if before['totalCPU']>400:
   data['blocked']=dict(reason='Background CPU exceeds 400% before trial',load=before)
   output.write_text(json.dumps(data,indent=2)+'\n');raise RuntimeError(data['blocked'])
  start=time.perf_counter_ns();packed=int(run([BUILD/program,'40',str(rows),str(count)]));elapsed=(time.perf_counter_ns()-start)/1e9
  total=mathematical_sum(40,count)
  r,value=divmod(packed,4294967296)
  assert 0<=r<10000 and value==total+r,(program,value,total,r)
  row=dict(program=program,kind=kind,block=block,rows=rows,inner=count,elapsedSeconds=elapsed,output=value,random=r,packedOutput=packed,backgroundBefore=before,backgroundAfter=load())
  data['scaling' if kind=='scaling' else 'observations'].append(row)
  output.write_text(json.dumps(data,indent=2)+'\n')
 for program in TIMED: trial(program,10000,100000,'warmup',-1)
 for rows in (2500,5000,10000):
  for program in ('kalandra','cpp','rust'):trial(program,rows,100000,'scaling',-1)
 rng=random.Random(20261031)
 for block in range(7):
  order=TIMED.copy();rng.shuffle(order)
  for program in order:trial(program,10000,100000,'measurement',block)
  print('Paired block',block+1,'complete',flush=True)
 assert frozen=={name:sha(BUILD/name) for name in TIMED},'Binary changed during timing'
 data['summary']={}
 for program in TIMED:
  values=[r['elapsedSeconds'] for r in data['observations'] if r['program']==program and r['kind']=='measurement']
  data['summary'][program]=dict(medianSeconds=statistics.median(values),minSeconds=min(values),maxSeconds=max(values),samples=len(values))
 data['identicalBinaryControlRatio']=data['summary']['cpp-repeat']['medianSeconds']/data['summary']['cpp']['medianSeconds']
 data['controlWithinFivePercent']=abs(data['identicalBinaryControlRatio']-1)<=0.05
 output.write_text(json.dumps(data,indent=2)+'\n')
 print(json.dumps(data['summary'],indent=2),flush=True)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('phase',choices=['build','verify','timing']);a=p.parse_args()
 {'build':build,'verify':verify,'timing':timing}[a.phase]()
