"""Isolate pipeline thread limits from strict matrix tile changes.

All calls dispatch exactly 256 threads. Baseline uses the existing register16
pipeline with Metal's automatic limit. Candidate explicitly limits compilation
to 256 threads, with either that same shader or a separately qualified tile.
No production sources change. No thread-execution-width hint is added.
"""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True)
parser.add_argument('--layout',choices=('register','adjacent','spaced','quad'),required=True)
parser.add_argument('--output',type=Path,required=True);parser.add_argument('--blocks',type=int,default=5)
a=parser.parse_args();assert not a.output.exists()
base=p.ROOT/'benchmarks/.build/gpu-output-reuse-2026-10-29'
original=base/('screen' if a.layout in ('register','adjacent') else a.layout)
build=base/('limit-'+a.layout);build.mkdir(parents=True,exist_ok=True)
driver_source=p.ROOT/'native/benchmarks/OutputReuseStudy.mm' if a.layout in ('register','adjacent') else original/'Driver.mm'
library=original/'study.metallib';binary=build/'screen';manifest=build/'manifest.json'
sources=[Path(__file__),driver_source,Path(__file__).parent/'fair/protocol.py',original/'manifest.json']
signature={p.key(f):p.sha(f) for f in sources+[library]}
if not manifest.exists():
 assert a.phase=='prepare';driver=driver_source.read_text()
 if a.layout=='register':driver=driver.replace('kalandra_matmul_hardware_pair32','kalandra_matmul_hardware_register16').replace('candidate?32:16','16')
 driver=driver.replace('auto make=[&](const char *name)', 'auto make=[&](const char *name,bool bounded)')
 old='auto p=[device newComputePipelineStateWithFunction:f error:&error]; check(p);'
 new='''id<MTLComputePipelineState> p;
    if(bounded) {auto descriptor=[[MTLComputePipelineDescriptor alloc] init];descriptor.computeFunction=f;descriptor.maxTotalThreadsPerThreadgroup=256;
      p=[device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&error];}
    else p=[device newComputePipelineStateWithFunction:f error:&error];check(p);
    fprintf(stderr,"pipeline %s bounded=%d max=%lu width=%lu shared=%lu\\n",name,bounded,(unsigned long)p.maxTotalThreadsPerThreadgroup,(unsigned long)p.threadExecutionWidth,(unsigned long)p.staticThreadgroupMemoryLength);'''
 assert driver.count(old)==1;driver=driver.replace(old,new)
 driver=driver.replace('make("kalandra_matmul_hardware_register16");','make("kalandra_matmul_hardware_register16",false);')
 old=': "kalandra_matmul_hardware_register16");';assert driver.count(old)==1
 driver=driver.replace(old,': "kalandra_matmul_hardware_register16",candidate);')
 (build/'Driver.mm').write_text(driver)
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',build/'Driver.mm','-framework','Foundation','-framework','Metal','-o',binary])
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in (binary,build/'Driver.mm',library)},commands=p.COMMANDS),indent=2)+'\n')
saved=json.loads(manifest.read_text());assert saved['signature']==signature
for f,h in saved['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(16,512,512),(64,512,2048),(64,2048,512),(16,512,2048),(16,2048,512),(64,512,512)]
if a.phase=='prepare':
 import subprocess
 checks=[];metadata=[]
 for shape in [(1,512,1),(3,512,17),(15,576,31),(17,1536,33),(17,4096,65),(33,2112,63),*shapes]:
  for pattern in range(3):
   for v in ('baseline','current'):
    command=list(map(str,[binary,library,v,*shape,1,1,pattern]))
    result=subprocess.run(command,capture_output=True,text=True,env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'),timeout=180)
    assert result.returncode==0,(command,result.stderr[-4000:])
    p.observations(result.stdout,shape[0]*shape[2],1,1)
    metadata+=result.stderr.splitlines();checks.append(dict(shape=shape,pattern=pattern,version=v,passed=True))
  print('Verified '+str(shape),flush=True)
 data=dict(contract=__doc__,layout=a.layout,checks=checks,pipeline_metadata=sorted(set(metadata)))
else:
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 q=build/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==72 and all(r['passed'] for r in qualified['checks'])
 def invoke(cell,count,rounds):return p.observations(p.run([binary,library,*cell,count,rounds,0]),cell[1]*cell[3],count,rounds)
 data=p.measure_paired([(v,*s) for s in shapes for v in ('baseline','current')],invoke,lambda c:c[1]*c[3],blocks=a.blocks,target_ms=30,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,layout=a.layout,qualification_sha256=p.sha(q),pipeline_metadata=qualified['pipeline_metadata'])
for f,h in saved['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
p.evidence(a.output,data,sources+[manifest],[binary,library,build/'Driver.mm'],p.ROOT/'build/native/native/kalandra')
