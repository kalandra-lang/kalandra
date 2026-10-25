"""Paired GPU submission study plus complete CPU/optimized-library pipeline controls."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import pipeline_reference as ref


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-metal',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--reuse-build',action='store_true')
    args=parser.parse_args()
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    for name in ('KALANDRA_GPU_PROFILE','KALANDRA_FAIR_PATTERN'):
        assert name not in os.environ,name+' must be unset'
    for name in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
        assert os.getenv(name) in (None,'','0'),name+' must be disabled'
    root=p.ROOT;src=Path(__file__).resolve().parent;fair=src/'fair';compiler=root/'build/native/native/kalandra'
    runtime=compiler.parent/'libkalandra-task-core-optimized.a';metal=compiler.parent/'metal/libkalandra-metal-optimized.a'
    build=p.BUILD.parent/'pipeline';build.mkdir(exist_ok=True);bundle=build/'bundle';openblas=Path('/opt/homebrew/opt/openblas')
    inputs=[Path(__file__),fair/'pipeline_reference.py',fair/'library_reference.py',fair/'protocol.py',src/'Pipeline.k',
            fair/'PipelineGpu.cpp',fair/'PipelineCpu.cpp',fair/'PipelineLibraries.cpp',compiler,runtime,metal,args.baseline_metal,
            openblas/'lib/libopenblas.dylib',root/'native/metal/Metal.mm']
    signature={str(x):p.sha(x) for x in inputs};manifest=build/'manifest.json'
    flags=['-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread','-Wno-override-module']
    if args.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature,'build inputs changed'
        assert all(p.sha(x)==h for x,h in saved['hashes'].items()),'build artifacts changed'
        p.COMMANDS.extend(saved['commands']);artifacts=list(map(Path,saved['artifacts']));generated=list(map(Path,saved['generated']))
    else:
        p.run([compiler,src/'Pipeline.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        artifacts=[];generated=[]
        for name,library in [('baseline',args.baseline_metal),('current',metal)]:
            binary=build/name
            p.run([p.CLANG,*flags,'-DBENCH_FLOAT32',fair/'PipelineGpu.cpp',library,runtime,'-framework','Foundation','-framework','Metal','-o',binary]);artifacts.append(binary)
        cpu_source=build/'cpu.k';cpu_source.write_text((src/'Pipeline.k').read_text().split('fn main()')[0]+'fn main()->float32:\n  0\n')
        ir=build/'cpu.ll';p.run([compiler,cpu_source,'--arithmetic','hardware','--test-entry','project','--emit-llvm',ir,'-O2','-o',build/'cpu-standalone'])
        p.run([p.CLANG,*flags,fair/'PipelineCpu.cpp',ir,runtime,'-o',build/'cpu']);artifacts.append(build/'cpu');generated += [cpu_source,ir]
        for name,link in [('openblas',['-I'+str(openblas/'include'),'-L'+str(openblas/'lib'),'-lopenblas','-Wl,-rpath,'+str(openblas/'lib')]),('accelerate',['-DACCELERATE','-DACCELERATE_NEW_LAPACK','-framework','Accelerate'])]:
            p.run([p.CLANG,*flags,'-DFLOAT32',fair/'PipelineLibraries.cpp',*link,'-o',build/name]);artifacts.append(build/name)
        artifacts += list(bundle.glob('*.metallib'));generated += list(bundle.glob('*.metal'))
        manifest.write_text(json.dumps(dict(signature=signature,commands=p.COMMANDS,artifacts=list(map(str,artifacts)),generated=list(map(str,generated)),hashes={str(x):p.sha(x) for x in artifacts+generated}),indent=2)+'\n')
    if args.build_only:
        print('Pipeline artifacts built; timing not run',flush=True);return
    # Qualify each optimized-library API independently before acceptance timing.
    def environment(threads):
        return dict(os.environ,OPENBLAS_NUM_THREADS=str(max(1,threads)),OMP_NUM_THREADS=str(max(1,threads)),OMP_DYNAMIC='FALSE')
    qualification=[]
    for backend,threads in [('openblas',1),('openblas',12),('accelerate',1),('accelerate',0)]:
        for n in (1,3,7,17,33):
            for pattern in (0,1):
                row=json.loads(p.run([build/backend,2,n,1,1,threads,pattern,1],environment(threads)))
                assert row['threads']==threads
                qualification.append(dict(backend=backend,threads=threads,n=n,pattern=pattern,**ref.assess(n,pattern,row['result'],row['cells'])))
    sizes=(128,256,512,1024,2048)
    expected={n:ref.exact(n) for n in sizes}
    def gpu(cell,count,rounds,environment=None):
        version,mode,n=cell
        return p.observations(p.run([build/version,bundle,int(mode=='transfers'),2,n,count,rounds],environment),expected[n],count,rounds)
    cells=[[v,mode,n] for v in ('baseline','current') for mode in ('resident','transfers') for n in sizes]
    result=dict(paired=p.measure_paired(cells,gpu,lambda c:expected[c[-1]],blocks=7,rounds=3,target_ms=10),qualification=qualification)
    controls=[['cpu',1,n] for n in sizes]
    controls += [[backend,threads,n] for backend,threads in [('openblas',1),('openblas',12),('accelerate',1),('accelerate',0)] for n in sizes]
    def control(cell,count,rounds):
        backend,threads,n=cell
        argv=[build/'cpu',n,count,rounds] if backend=='cpu' else [build/backend,2,n,count,rounds,threads,0,0]
        rows=p.observations(p.run(argv,environment(threads)),expected[n],count,rounds)
        if backend!='cpu':assert all(x['threads']==threads for x in rows)
        return rows
    result['controls']=p.measure(controls,control,lambda c:expected[c[-1]],processes=3,rounds=5,target_ms=20)
    result['gpu_profile']=[]
    for mode in ('resident','transfers'):
        for n in sizes:
            for version in ('baseline','current'):
                cell=[version,mode,n]
                result['gpu_profile'].append(dict(cell=cell,iterations=5,samples=gpu(cell,5,5,dict(os.environ,KALANDRA_GPU_PROFILE='1'))))
    result['contract']='Complete matmul -> bias -> scale -> ReLU -> balanced sum; exact dyadic runtime inputs. Fresh output/intermediate allocation, scheduling, dispatch and cleanup included. GPU scalar completion/readback included; transfers adds three uploads. Initialization, compilation and shader loading excluded. Same shaders/driver/runtime archive, only Metal implementation differs. CPU and BLAS controls are numerical API comparisons, not source-compiler rankings. BLAS can use different GEMM arithmetic/layout/hardware; chosen timed inputs have exact matrix entries. Libraries map and reduce their output in place; Kalandra owns its lowering strategy. No affinity/frequency/host isolation.'
    p.evidence(args.output,result,inputs+generated,artifacts,compiler)


if __name__=='__main__':main()
