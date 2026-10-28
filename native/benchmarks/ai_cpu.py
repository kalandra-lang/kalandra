"""Frozen-compiler paired CPU FFN experiments; historical studies stay untouched."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import ffn_reference as ref

CASES=[(1,32),(1,256),(2,256),(3,256),(4,256),(16,256),(128,256),
       (1,1024),(4,1024),(16,1024),(32,1024),(128,1024),(1,2048),(32,2048)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--baseline-dir',type=Path,default=p.ROOT/'benchmarks/.build/ai-sequence-2026-10-28/baseline')
    parser.add_argument('--compiler',type=Path,default=p.ROOT/'build/native/native/kalandra')
    parser.add_argument('--blocks',type=int,default=5)
    parser.add_argument('--target-ms',type=float,default=10)
    parser.add_argument('--reuse-build',action='store_true')
    parser.add_argument('--build-only',action='store_true')
    a=parser.parse_args();assert not a.output.exists()
    build=a.build_dir;build.mkdir(parents=True,exist_ok=True)
    fair=Path(__file__).parent/'fair';source=Path(__file__).with_name('ResidualFfn.k')
    task=a.compiler.parent/'libkalandra-task-core-optimized.a'
    sources=[Path(__file__),source,fair/'ResidualFfnCpu.cpp',fair/'ResidualFfn.h',fair/'ffn_reference.py',fair/'protocol.py',p.ROOT/'native/lib/Backend.cpp',p.ROOT/'native/lib/CheckedABI.cpp']
    baseline_manifest=json.loads((a.baseline_dir/'manifest.json').read_text())
    for name in ('cpu','cpu-dump','task.a','compiler'):
        assert p.sha(a.baseline_dir/name)==baseline_manifest[name]['sha256']
    signature={str(f):p.sha(f) for f in sources+[a.compiler,task]}
    manifest=build/'manifest.json'
    if a.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature
        assert all(p.sha(f)==h for f,h in saved['hashes'].items())
        p.COMMANDS.extend(saved['commands'])
    else:
        src=build/'cpu.k';src.write_text(source.read_text().split('fn main()')[0]+'fn main() -> float32:\n  0\n')
        generated=[src];artifacts=[]
        for name,entry,extra in [('cpu','project',[]),('cpu-dump','cells',['-DDUMP_CELLS'])]:
            ir=build/(name+'.ll')
            p.run([a.compiler,src,'--arithmetic','hardware','--test-entry',entry,'--emit-llvm',ir,'-O2','-o',build/(name+'-standalone')])
            p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread','-Wno-override-module','-DKALANDRA_CPU',*extra,fair/'ResidualFfnCpu.cpp',ir,task,'-o',build/name])
            generated.append(ir);artifacts.append(build/name)
        saved=dict(signature=signature,commands=p.COMMANDS,generated=list(map(str,generated)),artifacts=list(map(str,artifacts)),hashes={str(f):p.sha(f) for f in generated+artifacts})
        manifest.write_text(json.dumps(saved,indent=2)+'\n')
    if a.build_only:return
    binaries={'baseline':a.baseline_dir,'current':build}
    result=dict(verification=[],oracle_self_test=ref.self_test())
    expected={}
    verification=[(3,7,0),(3,7,1),(2,33,1),(1,257,1),(3,257,1)]+[(m,d,0) for m,d in CASES]
    for m,d,pattern in verification:
        for version,directory in binaries.items():
            row=json.loads(p.run([directory/'cpu-dump',m,d,1,1,1,pattern,1]))
            assessed=ref.assess(m,d,pattern,row,True)
            result['verification'].append(dict(version=version,m=m,d=d,pattern=pattern,**assessed))
            if not pattern:
                expected.setdefault((m,d),row['result']);assert expected[m,d]==row['result']
        print(f'Verified {m}x{d} pattern {pattern}',flush=True)
    def invoke(cell,count,rounds):
        version,m,d=cell
        return p.observations(p.run([binaries[version]/'cpu',m,d,count,rounds,1,0,0]),expected[m,d],count,rounds)
    cells=[[v,m,d] for v in binaries for m,d in CASES]
    result['paired']=p.measure_paired(cells,invoke,lambda c:expected[tuple(c[1:])],blocks=a.blocks,rounds=3,target_ms=a.target_ms,adaptive=True,max_iterations=1000)
    assert {str(f):p.sha(f) for f in sources+[a.compiler,task]}==signature
    assert all(p.sha(f)==h for f,h in saved['hashes'].items())
    result.update(contract='Same complete bias-free residual FFN source and strict float32 arithmetic. Frozen baseline compiler/runtime versus current compiler/runtime, same C++ driver. Fresh checked invocation, intermediate allocation, balanced scalar observation and cleanup included. Every timed shape and odd/non-dyadic controls receive independent full-cell bit validation before timing. Adjacent mirrored ABBA/BAAB fresh processes; no builds/tests/profilers run concurrently. Fixed dyadic timed data; no clocks/affinity/desktop isolation.',baseline_manifest=baseline_manifest,source_snapshots={p.key(f):f.read_text() for f in sources})
    p.evidence(a.output,result,sources+list(map(Path,saved['generated'])),list(map(Path,saved['artifacts']))+[task,a.baseline_dir/'cpu',a.baseline_dir/'cpu-dump',a.baseline_dir/'task.a'],a.compiler)

if __name__=='__main__':main()
