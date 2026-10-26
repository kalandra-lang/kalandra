"""Complete managed projection pipelines, including resident-weight activation transfers."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import pipeline_reference as ref

SHAPES=[(1,2048,2048),(1,4096,4096),(2,4096,4096),(1,8192,2048),(2,16384,1024),
        (1,544,257),(2,576,1027),(1,11008,1024),
        (4,4096,4096),(8,4096,4096),(128,128,128),(256,256,256),(512,512,512),(2048,2048,2048)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-metal',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--blocks',type=int,default=6)
    parser.add_argument('--reuse-build',action='store_true')
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--profile-only',action='store_true')
    parser.add_argument('--resident-only',action='store_true')
    args=parser.parse_args();assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    assert 'KALANDRA_GPU_PROFILE' not in os.environ
    assert all(os.getenv(k) in (None,'','0') for k in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'))
    root=p.ROOT;src=Path(__file__).resolve().parent;build=root/'benchmarks/.build/skinny-2026-10-26/pipeline';build.mkdir(parents=True,exist_ok=True)
    compiler=root/'build/native/native/kalandra';runtime=compiler.parent/'libkalandra-task-core-optimized.a';metal=compiler.parent/'metal/libkalandra-metal-optimized.a';bundle=build/'bundle'
    inputs=[Path(__file__),src/'Pipeline.k',src/'fair/PipelineGpu.cpp',src/'fair/pipeline_reference.py',src/'fair/protocol.py',
            root/'native/metal-source/SkinnyMatmul.h',root/'native/metal-source/HardwareShaders.h',root/'native/metal-source/GroupedMatmul.h',root/'native/metal-source/MatmulEpilogue.h',root/'native/metal-source/Build.cpp',root/'native/metal/Metal.mm',root/'native/metal/MatmulLayout.h',compiler,runtime,metal,args.baseline_metal]
    signature={str(x):p.sha(x) for x in inputs};manifest=build/'manifest.json'
    if args.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature
        assert all(p.sha(path)==h for path,h in saved['hashes'].items())
        p.COMMANDS.extend(saved['commands'])
    else:
        p.run([compiler,src/'Pipeline.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        for name,archive in [('baseline',args.baseline_metal),('current',metal)]:
            p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread','-DBENCH_FLOAT32',src/'fair/PipelineGpu.cpp',archive,runtime,'-framework','Foundation','-framework','Metal','-o',build/name])
        artifacts=[build/'baseline',build/'current',*bundle.glob('*.metallib'),*bundle.glob('*.metal')]
        manifest.write_text(json.dumps(dict(signature=signature,commands=p.COMMANDS,hashes={str(x):p.sha(x) for x in artifacts}),indent=2)+'\n')
    if args.build_only:
        print('Managed benchmark artifacts built; no timing run.');return
    modes=['resident'] if args.resident_only else ['resident','activations','transfers']
    expected={s:ref.exact_rect(*s) for s in SHAPES}
    def invoke(cell,count,rounds,environment=None):
        version,mode,*shape=cell
        return p.observations(p.run([build/version,bundle,{'resident':0,'transfers':1,'activations':2}[mode],2,*shape,count,rounds],environment),expected[tuple(shape)],count,rounds)
    cells=[[v,mode,*shape] for v in ('baseline','current') for mode in modes for shape in SHAPES]
    result=dict(contract='Complete matmul/bias/scale/ReLU/balanced sum with fresh intermediates, task scheduling, scalar readback and cleanup. Resident excludes initial three uploads. Activations keeps B resident and uploads A/bias per call; transfers uploads all three per call. Loading/compilation excluded. Same driver/task archive/shader bundle; only Metal runtime archive differs. Exact dyadic checksums in all warmup/calibration/timed rounds. No CPU affinity/frequency/desktop isolation. Dynamic budget max(64MiB,16*(M*K+K*N+M*N)+4096). Profiling is separate from acceptance.')
    if args.profile_only:
        result['profiles']=[]
        for block in range(args.blocks):
            for mode in modes:
                for shape in SHAPES:
                    for v in (('baseline','current') if block%2==0 else ('current','baseline')):
                        cell=[v,mode,*shape];result['profiles'].append(dict(cell=cell,block=block,iterations=3,samples=invoke(cell,3,3,dict(os.environ,KALANDRA_GPU_PROFILE='1'))))
    else:
        result['paired']=p.measure_paired(cells,invoke,lambda c:expected[tuple(c[-3:])],blocks=args.blocks,rounds=3,target_ms=20)
    p.evidence(args.output,result,inputs+list(bundle.glob('*.metal')),[build/'baseline',build/'current',*bundle.glob('*.metallib')],compiler)

if __name__=='__main__':main()
