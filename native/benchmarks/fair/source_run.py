"""Same source-level recursive algorithms in five languages; no tensor built-ins."""
import argparse,functools,json,math,os,sys
from fractions import Fraction
from pathlib import Path
import protocol as p
NAMES=('polynomial','branches','euclid')
@functools.cache
def expected(kind,depth,seed=17):
    count=1<<depth;lo=seed*count;hi=lo+count
    if kind==0:
        square=lambda n:n*(n+1)*(2*n+1)//6
        linear=(lo+hi-1)*count//2
        return float(Fraction(square(hi-1)-square(lo-1)+2*linear-4*count,8))
    if kind==1:
        return float(sum(Fraction(4*x if x%3==0 else -2*x if x%3==1 else 8*x+1,8) for x in range(lo,hi)))
    return float(sum(math.gcd(x*13+7,x*7+11) for x in range(lo,hi)))
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--depths',type=int,nargs='+',default=[10,14]);args=parser.parse_args()
    assert args.depths and len(set(args.depths))==len(args.depths) and all(0<=d<=14 for d in args.depths)
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    src=Path(__file__).resolve().parent;build=p.BUILD/'source';build.mkdir(parents=True,exist_ok=True)
    compiler=p.ROOT/'build/native/native/kalandra';env=dict(os.environ,GOMAXPROCS='1',GOCACHE=str(p.BUILD/'go-cache'))
    flags=['-std=c++23','-O2','-ffp-contract=off','-Wno-override-module','-pthread']
    p.run([p.CLANG,*flags,'-c',src/'Source.cpp','-o',build/'source.o'])
    p.run([p.CLANG,*flags,src/'SourceDriver.cpp',build/'source.o','-o',build/'cpp'])
    p.run([p.RUST,'--edition=2021','-C','opt-level=2','-C','lto=off',src/'source.rs','-o',build/'rust'])
    p.run(['go','build','-trimpath','-o',build/'go',src/'source.go'],env)
    for name in NAMES:
        ir=build/(name+'.ll')
        p.run([compiler,src/'source.k','--test-entry',name,'--emit-llvm',ir,'-O2','-o',build/(name+'-standalone')])
        text=ir.read_text();assert 'fmul contract' not in text and 'kalandra_test_entry' in text
        p.run([p.CLANG,*flags,'-DKALANDRA_KERNEL',src/'SourceDriver.cpp',ir,compiler.parent/'libkalandra-task-core-optimized.a','-o',build/('kalandra-'+name)])
    dependencies=[p.ROOT/name for name in ('native/lib/Lexer.cpp','native/lib/Parser.cpp','native/lib/Backend.cpp','native/lib/CheckedABI.cpp','native/lib/NativeFastABI.h','native/lib/NativeTailCalls.h','native/lib/NativeRecursiveContext.h', 'native/lib/NativeRecursiveDepth.h','native/lib/NumericConversion.cpp','native/include/kalandra/HardwareConversion.h','native/lib/NativeStack.h','native/runtime/Runtime.cpp','native/runtime/Runtime.h')]
    frozen={str(x):p.sha(x) for x in [compiler,compiler.parent/'libkalandra-task-core-optimized.a',*dependencies,*[x for x in src.iterdir() if x.is_file()]]}
    languages=('kalandra','cpp','go','rust','python')
    def invoke(cell,count,rounds,seed=17):
        lang,kind,depth=cell
        command=[sys.executable,src/'source.py'] if lang=='python' else [build/('kalandra-'+NAMES[kind] if lang=='kalandra' else lang)]
        return p.observations(p.run([*command,kind,depth,count,rounds,seed],env),expected(kind,depth,seed),count,rounds)
    checked=0
    for lang in languages:
        for kind in range(3):
            for depth in (0,1,3,9):
                for seed in (1,17,31):invoke([lang,kind,depth],1,1,seed);checked+=1
    print(f'{checked} cross-language edge/reference workloads passed',flush=True)
    cells=[[lang,k,d] for lang in languages for k in range(3) for d in args.depths]
    result=p.measure(cells,invoke,lambda cell:expected(cell[1],cell[2]))
    result.update(suite='source algorithms',verification_workloads=checked,
        contract='Same binary recursive traversal and leaf computation; all actual inputs fit int64 and dyadic results are exact float64. No tensor/reduction/matrix built-ins or numerical libraries. Kalandra includes checked arithmetic, invocation admission/cancellation and call-depth checks; other runtimes retain their default semantics. C++/Rust/Go may optimize recursion normally. No GPU general recursion support; GPU entries are unsupported, not replaced by built-ins.')
    sources=list(src.glob('*'));sources=[x for x in sources if x.is_file()]+dependencies
    binaries=[x for x in build.iterdir() if x.is_file()]+[compiler.parent/'libkalandra-task-core-optimized.a',Path(sys.executable)]
    assert all(p.sha(x)==h for x,h in frozen.items()),'benchmark sources changed during measurement'
    p.evidence(args.output,result,sources,binaries,compiler)
    lines=['# Equivalent source algorithm comparison','',result['contract'],'',
      'Three fresh processes per cell, two warmups and seven measured batches; median of process medians. Setup and process startup excluded; function execution and normal runtime overhead included. All warmup/measured checksums match independent integer/Fraction or standard GCD references exactly.','',
      '| Algorithm | Depth / leaves | Kalandra µs | C++ µs | Go µs | Rust µs | Python µs |','| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    table={tuple(r['cell']):r['median_ns']/1000 for r in result['summary']}
    for kind,name in enumerate(NAMES):
        for depth in args.depths:lines.append(f'| {name} | {depth} / {1<<depth:,} | '+' | '.join(f'{table[lang,kind,depth]:.3f}' for lang in languages)+' |')
    lines+=['','This is a narrow source/runtime benchmark, not a language-wide ranking. Safety checks, tail-call optimization, interpreter overhead and runtime invocation costs are intrinsic differences and are disclosed rather than artificially disabled. GPU is unsupported for these recursive source algorithms. Raw samples, commands, versions and hashes are in the adjacent JSON.']
    args.output.with_suffix('.md').write_text('\n'.join(lines)+'\n')
if __name__=='__main__':main()
