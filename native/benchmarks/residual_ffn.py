"""Complete residual feed-forward numerical API comparison on current artifacts."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import ffn_reference as ref

CASES=[(1,256),(16,256),(128,256),(1,1024),(32,1024),(128,1024),(1,2048),(32,2048)]
BACKENDS=[('cpu',1),('gpu',0),('gpu',1),('gpu',2),('openblas',1),('openblas',12),('accelerate',1),('accelerate',0)]

def environment(threads):
    return dict(os.environ,OPENBLAS_NUM_THREADS=str(max(1,threads)),OMP_NUM_THREADS=str(max(1,threads)),OMP_DYNAMIC='FALSE')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--build-dir',type=Path,default=p.ROOT/'benchmarks/.build/residual-ffn')
    parser.add_argument('--reuse-build',action='store_true')
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--processes',type=int,default=3)
    parser.add_argument('--rounds',type=int,default=5)
    parser.add_argument('--target-ms',type=float,default=30)
    parser.add_argument('--profile-only',action='store_true')
    parser.add_argument('--cpu-only',action='store_true',help='Refresh source CPU and optimized CPU-library controls only')
    args=parser.parse_args()
    backends=[cell for cell in BACKENDS if not args.cpu_only or cell[0]!='gpu']
    assert not (args.cpu_only and args.profile_only)
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    for key in ('KALANDRA_GPU_PROFILE','MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
        assert os.getenv(key) in (None,'','0'),key
    build=args.build_dir;build.mkdir(parents=True,exist_ok=True)
    root=p.ROOT;src=root/'native/benchmarks';fair=src/'fair'
    compiler=root/'build/native/native/kalandra'
    task=compiler.parent/'libkalandra-task-core-optimized.a'
    metal=compiler.parent/'metal/libkalandra-metal-optimized.a'
    blas=Path('/opt/homebrew/opt/openblas')
    sources=[Path(__file__),src/'ResidualFfn.k',fair/'ResidualFfn.h',fair/'ResidualFfnCpu.cpp',fair/'ResidualFfnGpu.cpp',fair/'ffn_reference.py',fair/'protocol.py',root/'native/metal/Metal.mm',root/'native/metal-source/Compiler.cpp',root/'native/metal-source/Build.cpp']
    signature={str(f):p.sha(f) for f in sources+[compiler,task,metal,blas/'lib/libopenblas.dylib']}
    manifest=build/'manifest.json';bundle=build/'bundle'
    if args.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature,'build inputs changed'
        assert all(p.sha(f)==h for f,h in saved['hashes'].items()),'artifact changed'
        p.COMMANDS.extend(saved['commands'])
    else:
        p.run([compiler,src/'ResidualFfn.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        host=(bundle/'host.cpp').read_text()
        assert 'products=std::array<m::MatrixProduct,4>{{{0,1},{3,3},{4,2},{0,5},}}' in host
        cpu_source=build/'cpu.k'
        cpu_source.write_text((src/'ResidualFfn.k').read_text().split('fn main()')[0]+'fn main() -> float32:\n  0\n')
        flags=['-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread','-Wno-override-module']
        for name,entry,extra in [('cpu','project',[]),('cpu-dump','cells',['-DDUMP_CELLS'])]:
            ir=build/(name+'.ll')
            p.run([compiler,cpu_source,'--arithmetic','hardware','--test-entry',entry,'--emit-llvm',ir,'-O2','-o',build/(name+'-standalone')])
            p.run([p.CLANG,*flags,'-DKALANDRA_CPU',*extra,fair/'ResidualFfnCpu.cpp',ir,task,'-o',build/name])
        p.run([p.CLANG,*flags,fair/'ResidualFfnGpu.cpp',metal,task,'-framework','Foundation','-framework','Metal','-o',build/'gpu'])
        for name,link in [('openblas',['-I'+str(blas/'include'),'-L'+str(blas/'lib'),'-lopenblas','-Wl,-rpath,'+str(blas/'lib')]),('accelerate',['-DACCELERATE','-DACCELERATE_NEW_LAPACK','-framework','Accelerate'])]:
            p.run([p.CLANG,*flags,fair/'ResidualFfnCpu.cpp',*link,'-o',build/name])
        artifacts=[build/name for name in ('cpu','cpu-dump','gpu','openblas','accelerate')]+list(bundle.glob('*.metallib'))
        generated=[cpu_source,build/'cpu.ll',build/'cpu-dump.ll',bundle/'host.cpp']+list(bundle.glob('*.metal'))
        saved=dict(signature=signature,commands=p.COMMANDS,artifacts=list(map(str,artifacts)),generated=list(map(str,generated)),hashes={str(f):p.sha(f) for f in artifacts+generated})
        manifest.write_text(json.dumps(saved,indent=2)+'\n')
    if args.build_only:
        print('Built; no measurements run',flush=True);return
    result=dict(oracle_self_test=ref.self_test(),verification=[])
    expected={}
    def command(backend,mode,m,d,count,rounds,pattern=0,dump=False):
        if backend=='gpu':return [build/'gpu',bundle,m,d,count,rounds,mode,pattern,int(dump)]
        binary='cpu-dump' if backend=='cpu' and dump else backend
        return [build/binary,m,d,count,rounds,mode,pattern,int(dump)]
    # Complete matrix verification on both adversarial odd shapes and every
    # timed shape. Non-dyadic inputs additionally qualify all backend variants.
    verification_cases=[(3,7,0),(3,7,1),(2,33,1),(1,257,1)]+[(m,d,0) for m,d in CASES]
    for m,d,pattern in verification_cases:
        for backend,mode in backends:
            row=json.loads(p.run(command(backend,mode,m,d,1,1,pattern,True),environment(mode)))
            assessed=ref.assess(m,d,pattern,row,backend in ('cpu','gpu'))
            result['verification'].append(dict(backend=backend,mode=mode,m=m,d=d,pattern=pattern,**assessed))
            if not pattern:expected[(backend,mode,m,d)]=row['result']
        print(f'Validated full {m}x{d} output, pattern {pattern}',flush=True)
    def invoke(cell,count,rounds,profile=False):
        backend,mode,m,d=cell
        env=environment(mode)
        if profile:env['KALANDRA_GPU_PROFILE']='1'
        rows=p.observations(p.run(command(*cell,count,rounds),env),expected[tuple(cell)],count,rounds)
        for row in rows:
            if backend=='gpu':
                assert row['submissions']==row['completions'] and row['submissions']%count==0
            else:assert row['threads']==mode
        return rows
    cells=[[backend,mode,m,d] for backend,mode in backends for m,d in CASES]
    if args.profile_only:
        result['profiles']=[dict(cell=c,iterations=3,samples=invoke(c,3,3,True)) for c in cells if c[0]=='gpu']
    else:
        result['measurement']=p.measure(cells,invoke,lambda c:expected[tuple(c)],processes=args.processes,rounds=args.rounds,target_ms=args.target_ms,adaptive=True,max_iterations=1000)
    assert {str(f):p.sha(f) for f in sources+[compiler,task,metal,blas/'lib/libopenblas.dylib']}==signature,'inputs changed during run'
    assert all(p.sha(f)==h for f,h in saved['hashes'].items())
    result['contract']='Bias-free residual feed-forward block: Y=ReLU(X W_up) W_down+X; dimensions MxD, Dx4D, 4DxD. Final adjacent-tree scalar sum is observed every call. Full cells validated before timing against independent separately-rounded binary32 product/tree oracle; optimized libraries may FMA/reassociate and are checked with propagated gamma bounds. Fresh invocation/task, intermediates, completion and cleanup included. GPU mode 0: all inputs resident; mode 1: weights resident, activation upload included; mode 2: all three uploads included. Initialization, shader/library loading, compilation and process startup excluded. GPU scalar readback included, full tensor download excluded from timing. CPU calls compiled Kalandra source; GPU uses its compiler-generated shaders and identical emitted graph through production managed runtime. OpenBLAS/Accelerate are optimized numerical API controls, not independent source-language compiler comparisons. Accelerate is reference-only, no production dependency. No full attention, normalization, tokenization, KV cache or model generation. No clocks/affinity/desktop isolation.'
    result['source_snapshots']={p.key(f):f.read_text() for f in sources}
    result['numpy_version']=ref.np.__version__
    p.evidence(args.output,result,sources+list(map(Path,saved['generated'])),list(map(Path,saved['artifacts']))+[task,metal,blas/'lib/libopenblas.dylib'],compiler)

if __name__=='__main__':main()
