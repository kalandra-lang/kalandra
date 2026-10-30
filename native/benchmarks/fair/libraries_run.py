"""Optimized numerical APIs; shared BLAS backend and allocation/thread policy explicit."""
import argparse,hashlib,json,os,re,sys,statistics,platform
from pathlib import Path
import protocol as p
import library_reference as ref
NAMES=('dot','axpy','matrix')
OPENBLAS=Path('/opt/homebrew/opt/openblas')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--verify-only',action='store_true');parser.add_argument('--reuse-build',action='store_true');parser.add_argument('--reuse-verification',action='store_true');args=parser.parse_args()
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
        assert os.environ.get(key) in (None,'','0'),key+' must be disabled for timing'
    src=Path(__file__).resolve().parent;old=src.parent/'languages';build=p.BUILD/'libraries';build.mkdir(parents=True,exist_ok=True)
    compiler=p.ROOT/'build/native/native/kalandra'
    base_env=dict(os.environ,GOMAXPROCS='1',GOCACHE=str(p.BUILD/'go-cache'))
    base_env.pop('KALANDRA_GPU_PROFILE',None);base_env.pop('KALANDRA_FAIR_PATTERN',None)
    binaries=[];generated=[]
    clang_flags=['-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-Wno-override-module','-pthread']
    build_signature={str(x):p.sha(x) for x in [compiler,compiler.parent/'libkalandra-task-core-optimized.a',OPENBLAS/'lib/libopenblas.dylib',*sorted(x for x in src.iterdir() if x.is_file()),old/'kernels.k',old/'gpu.k',old/'Driver.cpp',old/'GpuDriver.cpp',p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/runtime/Runtime.cpp',p.ROOT/'native/runtime/Runtime.h',p.ROOT/'native/lib/NativeFastABI.h',p.ROOT/'native/lib/NativeCallFoundation.h',p.ROOT/'native/lib/NumericConversion.cpp',p.ROOT/'native/include/kalandra/HardwareConversion.h',p.ROOT/'native/metal-source/HardwareShaders.h']}
    manifest=build/'manifest.json'
    if args.reuse_build:
        cached=json.loads(manifest.read_text());assert cached['signature']==build_signature,'build inputs changed'
        p.COMMANDS.extend(cached['build_commands'])
        binaries=[Path(x) for x in cached['binaries']];generated=[Path(x) for x in cached['generated']]
        assert all(p.sha(x)==h for x,h in cached['hashes'].items()),'compiled artifacts changed'
    else:
        for dtype in ('float32','float64'):
            directory=build/dtype;directory.mkdir(exist_ok=True)
            macros=['-DFLOAT32'] if dtype=='float32' else []
            bench_macros=['-DBENCH_FLOAT32'] if dtype=='float32' else []
            for backend in ('openblas','accelerate'):
                binary=directory/backend
                link=['-DACCELERATE','-DACCELERATE_NEW_LAPACK','-framework','Accelerate'] if backend=='accelerate' else ['-I'+str(OPENBLAS/'include'),'-L'+str(OPENBLAS/'lib'),'-lopenblas','-Wl,-rpath,'+str(OPENBLAS/'lib')]
                p.run([p.CLANG,*clang_flags,*macros,src/'Libraries.cpp',*link,'-o',binary]);binaries.append(binary)
            rust=directory/'libraries.rs';text=(src/'libraries.rs').read_text()
            if dtype=='float32':text=re.sub(r'type Scalar\s*=\s*f64','type Scalar=f32',text).replace('cblas_d','cblas_s')
            rust.write_text(text);generated.append(rust)
            p.run([p.RUST,'--edition=2021','-C','opt-level=2','-C','lto=off','-L',str(OPENBLAS/'lib'),'-C','link-arg=-Wl,-rpath,'+str(OPENBLAS/'lib'),rust,'-o',directory/'rust']);binaries.append(directory/'rust')
            go=directory/'libraries.go';text=(src/'libraries.go').read_text()
            if dtype=='float32':text=re.sub(r'Scalar\s*=\s*float64','Scalar=float32',text).replace('C.double','C.float').replace('cblas_d','cblas_s')
            go.write_text(text);generated.append(go)
            env=dict(base_env,CGO_ENABLED='1',CGO_CFLAGS='-O2 -I'+str(OPENBLAS/'include'),CGO_LDFLAGS='-L'+str(OPENBLAS/'lib')+' -lopenblas -Wl,-rpath,'+str(OPENBLAS/'lib'))
            p.run(['go','build','-trimpath','-o',directory/'go',go],env);binaries.append(directory/'go')
            for driver in ('Driver.cpp','GpuDriver.cpp'):
                source=old/driver;text=source.read_text()
                text=re.sub(r'#include "([^"]+)"',lambda m:'#include "'+str((source.parent/m[1]).resolve())+'"',text)
                text='#include "'+str(src/'FairInputs.h')+'"\n'+text
                text=text.replace('(int(i % 17) - 8) / 8.0','fairA<Float>(i)').replace('(int(i % 13) - 6) / 8.0','fairB<Float>(i)')
                dest=directory/driver;dest.write_text(text);generated.append(dest)
            for name in ('kernels.k','gpu.k'):
                dest=directory/name;dest.write_text((old/name).read_text().replace('float64',dtype));generated.append(dest)
            arithmetic='hardware' if dtype=='float32' else 'hardware-with-software-fallback'
            for kind,name in enumerate(NAMES):
                ir=directory/(name+'.ll');generated.append(ir)
                p.run([compiler,directory/'kernels.k','--arithmetic',arithmetic,'--test-entry',name,'--emit-llvm',ir,'-O2','-o',directory/(name+'-standalone')])
                assert re.search(r'\b(fadd|fmul)\b',ir.read_text())
                binary=directory/('kalandra-'+name)
                p.run([p.CLANG,*clang_flags,*bench_macros,'-DKALANDRA_KERNEL',*(['-DMATRIX_KERNEL'] if kind==2 else []),directory/'Driver.cpp',ir,compiler.parent/'libkalandra-task-core-optimized.a','-o',binary]);binaries.append(binary)
            p.run([compiler,directory/'gpu.k','--target','metal','--arithmetic',arithmetic,'-O2','-o',directory/'gpu-bundle'])
            objects=[]
            for i,source in enumerate((directory/'GpuDriver.cpp',p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/runtime/Runtime.cpp',p.ROOT/'native/runtime/Tasks.cpp')):
                obj=directory/f'gpu-{i}.o';objects.append(obj)
                extra=['-fobjc-arc','-fblocks','-fno-objc-msgsend-class-selector-stubs'] if source.suffix=='.mm' else []
                p.run([p.CLANG,*clang_flags,*bench_macros,*extra,'-c',source,'-o',obj])
            p.run([p.CLANG,'-O2',*objects,'-framework','Foundation','-framework','Metal','-o',directory/'gpu']);binaries.append(directory/'gpu')
            generated+=list((directory/'gpu-bundle').glob('*.metallib'))+objects
        manifest.write_text(json.dumps(dict(signature=build_signature,build_commands=list(p.COMMANDS),binaries=list(map(str,binaries)),generated=list(map(str,generated)),hashes={str(x):p.sha(x) for x in binaries+generated}),indent=2))
    print('All native/BLAS bindings compiled',flush=True)
    variants=('kalandra-cpu','kalandra-gpu-resident','kalandra-gpu-transfers','cpp-openblas','go-openblas','rust-openblas','python-openblas','numpy','cpp-accelerate')
    def config_env(threads,pattern):
        env=dict(base_env,OPENBLAS_NUM_THREADS=str(max(1,threads)),OMP_NUM_THREADS=str(max(1,threads)),OMP_DYNAMIC='FALSE')
        if pattern:env['KALANDRA_FAIR_PATTERN']='1'
        return env
    def invoke(cell,count,rounds,pattern=0,dump=False):
        dtype,variant,threads,kind,n=cell;directory=build/dtype;env=config_env(threads,pattern)
        if variant=='kalandra-cpu':argv=[directory/('kalandra-'+NAMES[kind]),kind,n,count,rounds]
        elif variant.startswith('kalandra-gpu'):argv=[directory/'gpu',directory/'gpu-bundle',int(variant.endswith('transfers')),kind,n,count,rounds]
        elif variant.startswith('python') or variant=='numpy':argv=[sys.executable,src/'libraries.py',dtype,'numpy' if variant=='numpy' else 'ctypes',kind,n,count,rounds,threads,pattern,int(dump)]
        else:
            name={'cpp-openblas':'openblas','cpp-accelerate':'accelerate','go-openblas':'go','rust-openblas':'rust'}[variant]
            argv=[directory/name,kind,n,count,rounds,threads,pattern,int(dump)]
        text=p.run(argv,env)
        if dump:return json.loads(text)
        if pattern:
            rows=[json.loads(line) for line in text.splitlines()]
            assert [r['round'] for r in rows]==list(range(-2,rounds))
            return rows
        rows=p.observations(text,ref.exact(kind,n),count,rounds)
        if not variant.startswith('kalandra'):assert all(r['threads']==threads for r in rows)
        return rows
    qualification=build/'qualification.json'
    if args.reuse_verification:
        checked=json.loads(qualification.read_text());assert checked['signature']==build_signature,'qualification inputs changed'
        assert all(p.sha(x)==h for x,h in json.loads(manifest.read_text())['hashes'].items()),'qualified artifacts changed'
        verification=checked['verification']
    else:
        verification=[]
        # All binding outputs, including per-cell arrays, before accepting any timings.
        for dtype in ('float32','float64'):
            for variant in variants:
                modes=(1,) if variant.startswith('kalandra') else (1,0) if variant=='cpp-accelerate' else (1,12)
                for threads in modes:
                    for kind in range(3):
                        for n,pattern in ((1,0),(3,0),(17,0),(3,1),(17,1)):
                            cell=[dtype,variant,threads,kind,n]
                            if variant.startswith('kalandra'):
                                rows=invoke(cell,1,1,pattern)
                                errors=[ref.assess(kind,n,dtype,pattern,float(r['checksum'])) for r in rows]
                                result=max(errors,key=lambda r:r['max_absolute_error'])
                            else:
                                row=invoke(cell,0,0,pattern,True);assert row['threads']==threads
                                result=ref.assess(kind,n,dtype,pattern,float(row['result']),row['cells'])
                            verification.append(dict(cell=cell,pattern=pattern,**result))
                    print(f'{dtype} {variant} threads={threads}: accuracy verified',flush=True)
        qualification.write_text(json.dumps(dict(signature=build_signature,verification=verification),indent=2))
    if args.verify_only:
        args.output.write_text(json.dumps(dict(signature=build_signature,verification=verification),indent=2)+'\n')
        print('Numerical qualification saved: '+str(args.output),flush=True)
        return
    # Reproducible accuracy: u=2^-24/2^-53 and gamma(ops)=ops*u/(1-ops*u),
    # with exact Decimal products and absolute-sum scales; no empirical tolerance tuning.
    cells=[]
    for dtype in ('float32','float64'):
        workloads=[(k,n) for k in (0,1) for n in (262144,4194304)]+[(2,n) for n in ((256,512,1024) if dtype=='float32' else (256,512))]
        for variant in variants:
            modes=(1,) if variant.startswith('kalandra') else (1,0) if variant=='cpp-accelerate' else (1,12)
            for threads in modes:
                cells += [[dtype,variant,threads,k,n] for k,n in workloads]
    print(f'{len(verification)} independent numerical workloads passed; timing {len(cells)} cells',flush=True)
    result=p.measure(cells,invoke,lambda cell:ref.exact(cell[3],cell[4]))
    result.update(suite='optimized numerical libraries',verification=verification,library_config=p.run([sys.executable,'-c',"import ctypes as c,numpy as np,json; l=c.CDLL('/opt/homebrew/opt/openblas/lib/libopenblas.dylib');l.openblas_get_config.restype=c.c_char_p;print(json.dumps(dict(openblas=l.openblas_get_config().decode(),numpy=np.__version__,numpy_module=np._core._multiarray_umath.__file__)))"]),
      contract='Whole dot / AXPY+sum / GEMM+sum tasks. Fresh output allocation and language dispatch included; setup/input initialization/library loading excluded. C++/Rust/Python use uninitialized fresh output where APIs permit; Go uses zero-initialized make and natural GC. Sum uses optimized BLAS dot with preinitialized ones for BLAS bindings, NumPy sum for NumPy. Shared OpenBLAS backend across C++/Go/Rust/Python/NumPy; these are API/binding comparisons, not independent language kernels. Explicit queried 1/12 OpenBLAS thread modes; Accelerate SINGLE/AUTO are queried, AUTO is not a claimed fixed thread count. Kalandra CPU is single-thread; GPU resident/transfer boundaries retained. Float64 GPU is explicitly software fallback; float32 GPU is hardware. Timed dyadic inputs require exact batch checksums. Non-dyadic verification permits justified gamma-based rounding error without changing Kalandra arithmetic semantics.')
    sources=[x for x in src.iterdir() if x.is_file()]+[old/'kernels.k',old/'gpu.k',old/'Driver.cpp',old/'GpuDriver.cpp']+[p.ROOT/name for name in ('native/lib/Backend.cpp','native/lib/CheckedABI.cpp','native/lib/NativeFastABI.h','native/lib/NativeCallFoundation.h','native/lib/NumericConversion.cpp','native/include/kalandra/HardwareConversion.h','native/lib/NativeStack.h','native/runtime/Runtime.cpp','native/runtime/Runtime.h','native/metal/Metal.mm','native/metal/Metal.h','native/metal-source/Build.cpp','native/metal-source/HardwareShaders.h')]
    assert all(p.sha(x)==h for x,h in build_signature.items()),'benchmark inputs changed during measurement'
    p.evidence(args.output,result,sources,binaries+generated+[OPENBLAS/'lib/libopenblas.dylib',compiler.parent/'libkalandra-task-core-optimized.a',Path(sys.executable),Path(json.loads(result['library_config'])['numpy_module'])],compiler)
    lines=['# Optimized numerical API comparison','',result['contract'],'',
      'Three fresh measured processes per cell, two warmups and seven measured batches, median of process medians. Every timed/warmup checksum is independently exact; non-dyadic per-cell/scalar accuracy is checked before timing. The JSON retains all verification errors/bounds and actual thread settings.','',
      '| Type | Operation | Size | API/backend | Thread mode | Median µs |','| --- | --- | ---: | --- | --- | ---: |']
    for r in sorted(result['summary'],key=lambda r:(r['cell'][0],r['cell'][3],r['cell'][4],r['cell'][1],r['cell'][2])):
        dtype,variant,threads,kind,n=r['cell'];mode='GPU' if 'gpu' in variant else 'AUTO' if threads==0 else str(threads)
        lines.append(f'| {dtype} | {NAMES[kind]} | {n:,} | {variant} | {mode} | {r["median_ns"]/1000:.3f} |')
    lines+=['','These are controlled numerical API workloads, not a language-wide ranking. OpenBLAS-backed wrappers share their actual optimized kernel. Accelerate AUTO can choose its own worker count. CPU single-thread, configured multi-thread and GPU results are explicitly separate. GPU transfers and library arithmetic contracts must be considered when choosing an implementation. No CPU affinity, frequency control or machine isolation was applied.']
    args.output.with_suffix('.md').write_text('\n'.join(lines)+'\n')
if __name__=='__main__':main()
