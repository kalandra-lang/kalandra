"""Matched checked source calls with and without a bound cancellation token.
Run after the fair source suite; all arithmetic/checks remain enabled.
"""
import argparse,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import source_run as source
import library_reference as ref

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cpu-only',action='store_true');args=parser.parse_args()
    assert not args.output.exists()
    directory=p.BUILD.parent/'checked-call-control';directory.mkdir(exist_ok=True)
    original=p.ROOT/'native/benchmarks/fair/SourceDriver.cpp'
    text=original.read_text()
    text=re.sub(r'#include "([^"]+)"',lambda m:'#include "'+str((original.parent/m[1]).resolve())+'"',text)
    text='#include <atomic>\n'+text
    text=text.replace('  for (int round = -2;', '  std::atomic<bool> cancellation{false};\n  for (int round = -2;')
    text=text.replace('kalandra::runtime::Invocation context;', 'kalandra::runtime::Invocation context;\n      context.bindCancellation(&cancellation);')
    driver=directory/'BoundDriver.cpp';driver.write_text(text)
    compiler=p.ROOT/'build/native/native/kalandra';runtime=compiler.parent/'libkalandra-task-core-optimized.a'
    for name in source.NAMES:
        p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-Wno-override-module','-pthread','-DKALANDRA_KERNEL',driver,p.BUILD/'source'/(name+'.ll'),runtime,'-o',directory/name])
    def invoke(cell,count,rounds):
        name,mode=cell;kind=source.NAMES.index(name)
        binary=directory/name if mode=='bound' else p.BUILD/'source'/('kalandra-'+name)
        return p.observations(p.run([binary,kind,14,count,rounds,17]),source.expected(kind,14),count,rounds)
    result=p.measure([[name,mode] for name in source.NAMES for mode in ('unbound','bound')],invoke,lambda cell:source.expected(source.NAMES.index(cell[0]),14),rounds=5)
    gpu=[]
    env=dict(os.environ,KALANDRA_GPU_PROFILE='1')
    for trial in range(0 if args.cpu_only else 3):
        for kind,n in ((0,4194304),(1,4194304),(2,512),(2,1024)):
            for transfers in (0,1):
                count=8 if kind==2 else 40
                rows=p.observations(p.run([p.BUILD/'libraries/float32/gpu',p.BUILD/'libraries/float32/gpu-bundle',transfers,kind,n,count,5],env),ref.exact(kind,n),count,5)
                gpu.append(dict(process=trial,kind=kind,n=n,transfers=transfers,iterations=count,samples=rows))
                print('GPU counters',trial,kind,n,transfers,flush=True)
    result['gpu_profile']=gpu
    result['contract']='Same final Kalandra LLVM/runtime and source traversal; bound uses a non-cancelled atomic token via normal bindCancellation. Binding, invocation lifecycle and acquire-load checks included. Independent correctness tests cancel the fast token path; this control measures its cost without diagnostic poll counters. Not a cross-language ranking.'
    sources=[Path(__file__),original,driver,*[p.ROOT/name for name in ('native/runtime/Runtime.h','native/runtime/Runtime.cpp','native/lib/CheckedABI.cpp','native/lib/NativeFastABI.h','native/lib/NativeCallFoundation.h','native/lib/NativeStack.h','native/lib/NumericConversion.cpp','native/include/kalandra/HardwareConversion.h')]]
    binaries=[runtime,*[directory/name for name in source.NAMES],*[p.BUILD/'source'/('kalandra-'+name) for name in source.NAMES]]
    if not args.cpu_only:
        result['contract']+=' GPU counters are separate instrumented float32 runs including complete task wall time, command-buffer device duration, host encoding and dispatch/submission counts.'
        sources += [p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/metal-source/HardwareShaders.h']
        binaries += [p.BUILD/'libraries/float32/gpu',*list((p.BUILD/'libraries/float32/gpu-bundle').glob('*.metallib'))]
    p.evidence(args.output,result,sources,binaries,compiler)
if __name__=='__main__':main()
