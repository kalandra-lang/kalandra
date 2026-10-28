"""Complete source-level transformer block versus a benchmark-only MPSGraph control."""
import argparse
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
os.environ['OPENBLAS_NUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import transformer_reference as ref
import prepared_program

CASES=[(1,64),(16,64),(64,64),(1,256),(16,256),(64,256),(1,512),(16,512),(64,512)]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--blocks',type=int,default=5)
    parser.add_argument('--target-ms',type=float,default=30)
    parser.add_argument('--reuse-build',action='store_true')
    parser.add_argument('--verify-only',action='store_true')
    parser.add_argument('--profile-only',action='store_true')
    parser.add_argument('--acceptance-evidence',type=Path,help='Verified timing evidence to instrument with --profile-only')
    args=parser.parse_args();assert not args.output.exists()
    if args.profile_only:
        if not args.acceptance_evidence:parser.error('--profile-only requires --acceptance-evidence; counters must be explicitly enabled in a separate driver')
        subprocess.run([sys.executable,str(Path(__file__).with_name('transformer_profile.py')),'--evidence',str(args.acceptance_evidence),'--build-dir',str(args.build_dir/'profile'),'--output',str(args.output)],check=True)
        return
    for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
        assert os.getenv(flag) in (None,'','0'),flag
    assert args.profile_only or os.getenv('KALANDRA_GPU_PROFILE') in (None,'','0')
    root=p.ROOT;build=args.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
    fair=Path(__file__).parent/'fair';compiler=root/'build/native/native/kalandra'
    native=compiler.parent/'libkalandra-native-runtime.a';task=compiler.parent/'libkalandra-task-core-optimized.a';metal=compiler.parent/'metal/libkalandra-metal-optimized.a'
    source=Path(__file__).parent/'TransformerBlock.k';bundle=build/'bundle'
    sources=[Path(__file__),source,*[fair/f for f in ('TransformerBlock.h','TransformerCpu.cpp','TransformerGpu.cpp','TransformerMps.mm','ResidualFfn.h','ffn_reference.py','transformer_reference.py','protocol.py','prepared_program.py')],*[root/f for f in ('native/lib/Backend.cpp','native/lib/CheckedABI.cpp','native/metal/Metal.mm','native/metal/Metal.h','native/metal-source/Compiler.cpp','native/metal-source/Build.cpp','native/metal-source/MatmulEpilogue.h','native/metal-source/NumericProgram.cpp','native/metal-source/NumericShader.cpp','native/metal-source/TensorTransformShader.h','native/include/kalandra/TensorTransform.h')]]
    signature={str(f):p.sha(f) for f in sources+[compiler,native,task,metal]}
    manifest=build/'manifest.json'
    if args.reuse_build:
        old=json.loads(manifest.read_text());assert old['signature']==signature
        assert all(p.sha(f)==h for f,h in old['hashes'].items())
        p.COMMANDS.extend(old['commands'])
    else:
        p.run([compiler,source,'--test-entry','block','--arithmetic','hardware','-O2','--emit-llvm',build/'cpu.ll','-o',build/'cpu-standalone'])
        p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-pthread','-Wno-override-module',fair/'TransformerCpu.cpp',build/'cpu.ll',native,'-o',build/'cpu'])
        gpu_source=build/'block-gpu.k'
        gpu_source.write_text(source.read_text().split('fn main()')[0]+'''fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    cpu=devices.cpu()
    x:Tensor[float32,2,2]=tensor([[1,2],[3,4]])
    w:Tensor[float32,2,2]=tensor([[1,0],[0,1]])
    up:Tensor[float32,2,8]=tensor([1,0,1,0,1,0,1,0,0,1,0,1,0,1,0,1],[2,8])
    down:Tensor[float32,8,2]=tensor([1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1],[8,2])
    a=try await transfer(x,gpu)
    b=try await transfer(w,gpu)
    c=try await transfer(up,gpu)
    d=try await transfer(down,gpu)
    y=try await spawn block(a,b,b,b,b,c,d,0.00001,0.70710677) on gpu
    try await transfer(y,cpu)
''')
        p.run([compiler,gpu_source,'--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        host=(bundle/'host.cpp').read_text()
        assert host.count('m::NumericInstructionKind::Map,')==8 and host.count('m::NumericInstructionKind::Matmul,')==8
        text=prepared_program.header(host)
        (build/'TransformerProgram.h').write_text(text)
        p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-I'+str(build),fair/'TransformerGpu.cpp',metal,task,'-framework','Foundation','-framework','Metal','-o',build/'gpu'])
        p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-ffp-contract=off','-mmacosx-version-min=26.0',fair/'TransformerMps.mm','-framework','Foundation','-framework','Metal','-framework','MetalPerformanceShaders','-framework','MetalPerformanceShadersGraph','-o',build/'mps'])
        artifacts=[build/'cpu',build/'gpu',build/'mps',build/'cpu.ll',build/'TransformerProgram.h',gpu_source,bundle/'host.cpp',*bundle.glob('*.metallib')]
        manifest.write_text(json.dumps(dict(signature=signature,hashes={str(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
    hashes=json.loads(manifest.read_text())['hashes']
    diagnostics={}
    def execute(backend,m,d,mode,count=1,rounds=1,dump=0,pattern=0):
        command=[build/backend,*([bundle] if backend=='gpu' else []),m,d,count,rounds,0 if backend=='cpu' else mode,dump,pattern]
        p.COMMANDS.append(list(map(str,command)))
        process=subprocess.run(list(map(str,command)),cwd=root,text=True,capture_output=True,timeout=240)
        assert not process.returncode,(command,process.returncode,process.stderr[-4000:])
        if process.stderr:diagnostics.setdefault(backend,{})[process.stderr[-4096:]]=diagnostics.get(backend,{}).get(process.stderr[-4096:],0)+1
        if dump:return json.loads(process.stdout)
        return p.observations(process.stdout,m*d*4,count,rounds)
    verification=[]
    for m,d,pattern in [(2,3,1),(3,7,1),(5,17,1),(17,33,1)]+[(m,d,0) for m,d in CASES]:
        cpu=None
        for backend in ('cpu','gpu','mps'):
            for mode in ((0,) if backend=='cpu' else (0,1,2)):
                row=execute(backend,m,d,mode,dump=1,pattern=pattern)
                metrics,cells=ref.assess(m,d,pattern,row)
                if backend=='cpu':cpu=cells
                metrics.update(backend=backend,m=m,d=d,mode=mode,pattern=pattern,max_cpu_difference=float(ref.np.max(ref.np.abs(cells.astype(ref.np.float64)-cpu.astype(ref.np.float64)))))
                verification.append(metrics)
        print(f'{m}x{d} pattern {pattern}: every cell verified on CPU, Metal, and MPSGraph in all residency modes',flush=True)
    calibration=[];observations=[];counts={};summary=[]
    if not args.verify_only:
        for m,d in CASES:
            for backend in ('cpu','gpu','mps'):
                for mode in ((0,) if backend=='cpu' else (0,1,2)):
                    count=1
                    for _ in range(8):
                        rows=execute(backend,m,d,mode,count,3);cost=statistics.median(row['ns'] for row in rows if row['round']>=0)
                        calibration.append(dict(backend=backend,m=m,d=d,mode=mode,iterations=count,samples=rows))
                        if cost>=args.target_ms*1e6*.8 or count>=50000:break
                        count=min(50000,max(count+1,int(count*min(16,args.target_ms*1e6/max(1,cost)))))
                    counts[backend,m,d,mode]=count
        groups=[(m,d,mode) for m,d in CASES for mode in (0,1,2)];rng=random.Random(20261028)
        for block in range(args.blocks):
            order=groups.copy();rng.shuffle(order)
            for m,d,mode in order:
                versions=('cpu','gpu','mps','mps','gpu','cpu') if not block%2 else ('mps','gpu','cpu','cpu','gpu','mps')
                medians={name:[] for name in ('cpu','gpu','mps')}
                for slot,backend in enumerate(versions):
                    count=counts[backend,m,d,0 if backend=='cpu' else mode]
                    rows=execute(backend,m,d,mode,count,3)
                    value=statistics.median(row['ns']/count for row in rows if row['round']>=0);medians[backend].append(value)
                    observations.append(dict(backend=backend,m=m,d=d,mode=mode,process=block,slot=slot,iterations=count,samples=rows))
                print(f'{block+1}/{args.blocks} {m}x{d} mode {mode}: CPU/Metal/MPS '+ '/'.join(f'{statistics.mean(medians[name])/1000:.1f}us' for name in ('cpu','gpu','mps')),flush=True)
        for m,d,mode in groups:
            for backend in ('cpu','gpu','mps'):
                medians=[statistics.median(s['ns']/row['iterations'] for s in row['samples'] if s['round']>=0) for row in observations if (row['backend'],row['m'],row['d'],row['mode'])==(backend,m,d,mode)]
                summary.append(dict(backend=backend,m=m,d=d,mode=mode,median_ns=statistics.median(medians),process_medians_ns=medians))
    assert all(p.sha(f)==h for f,h in hashes.items()) and all(p.sha(f)==h for f,h in signature.items())
    data=dict(contract=__doc__,model='Bias-free single-head unmasked pre-RMSNorm encoder; separate Q/K/V and output projections; attention softmax; two residuals; explicit epsilon and scale; unit RMS gain; 4x ReLU FFN. No KV cache, masks, tokenizer or generation.',timing='Warm synchronous tensor-result API, complete device ACK and invocation cleanup; no output readback in timing on either GPU. Pipeline/graph preparation excluded. Mode 0 all resident; mode 1 activation upload; mode 2 all seven inputs uploaded per call. CPU includes native invocation ownership cleanup. Fresh processes, two warmup batches, three measured batches, reversed CPU/GPU/MPS/MPS/GPU/CPU orders; implementation-specific calibrated counts avoid huge duration skew.',protocol=dict(blocks=args.blocks,target_ms=args.target_ms,seed=20261028),profile_only=args.profile_only,verification=verification,diagnostics=diagnostics,calibration=calibration,observations=observations,summary=summary)
    p.evidence(args.output,data,sources,[compiler,native,task,metal,*map(Path,hashes)],compiler)
if __name__=='__main__':main()
