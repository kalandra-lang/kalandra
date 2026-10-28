"""Paired managed GPU FFN: same frozen shaders, old/new production runtimes."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import ffn_reference as ref

CASES=[(1,256),(16,256),(128,256),(1,1024),(32,1024),(128,1024),(1,2048),(32,2048)]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--build-dir',required=True,type=Path)
    parser.add_argument('--blocks',type=int,default=5)
    parser.add_argument('--target-ms',type=float,default=30)
    parser.add_argument('--reuse-build',action='store_true')
    parser.add_argument('--profile-only',action='store_true')
    args=parser.parse_args();assert not args.output.exists()
    for name in ('KALANDRA_GPU_PROFILE','MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):
        assert os.getenv(name) in (None,'','0'),name
    build=args.build_dir;build.mkdir(parents=True,exist_ok=True)
    root=p.ROOT;fair=Path(__file__).parent/'fair';compiler=root/'build/native/native/kalandra'
    baseline=root/'benchmarks/.build/ai-sequence-2026-10-28/baseline'
    task=compiler.parent/'libkalandra-task-core-optimized.a'
    metal=compiler.parent/'metal/libkalandra-metal-optimized.a'
    bundle=root/'benchmarks/.build/residual-ffn/bundle'
    old=json.loads((root/'benchmarks/results/residual-ffn-2026-10-28.json').read_text())
    shaders=list(bundle.glob('*.metallib'))
    assert shaders and all(p.sha(f)==old['artifact_sha256'][p.key(f)] for f in shaders)
    sources=[Path(__file__),fair/'ResidualFfnGpu.cpp',fair/'ResidualFfn.h',fair/'ffn_reference.py',fair/'protocol.py',root/'native/benchmarks/ResidualFfn.k',root/'native/metal/Metal.mm',root/'native/metal/Metal.h',bundle/'host.cpp']
    signature={str(f):p.sha(f) for f in sources+[compiler,task,metal,baseline/'metal.a',*shaders]}
    manifest=build/'manifest.json'
    if args.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature
        assert all(p.sha(f)==h for f,h in saved['hashes'].items())
        p.COMMANDS.extend(saved['commands'])
    else:
        driver=build/'Driver.cpp'
        text=(fair/'ResidualFfnGpu.cpp').read_text()
        needle='resident_bytes\\\":%llu}\\n'
        assert text.count(needle)==1
        text=text.replace(needle,'resident_bytes\\\":%llu,\\\"attempts\\\":%llu}\\n')
        needle='(unsigned long long)baseline);'
        assert text.count(needle)==1
        text=text.replace(needle,'(unsigned long long)baseline,(unsigned long long)(after.attempts-before.attempts));')
        driver.write_text(text)
        for version,archive in [('baseline',baseline/'metal.a'),('current',metal)]:
            p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread','-I'+str(fair),driver,archive,task,'-framework','Foundation','-framework','Metal','-o',build/version])
        saved=dict(signature=signature,commands=p.COMMANDS,driver=str(driver),hashes={str(f):p.sha(f) for f in [driver,build/'baseline',build/'current']})
        manifest.write_text(json.dumps(saved,indent=2)+'\n')
    expected={};result=dict(oracle_self_test=ref.self_test(),verification=[])
    for m,d,pattern in [(3,7,1),(2,33,1),(1,257,1)]+[(m,d,0) for m,d in CASES]:
        for version in ('baseline','current'):
            for mode in range(3):
                row=json.loads(p.run([build/version,bundle,m,d,1,1,mode,pattern,1]))
                result['verification'].append(dict(version=version,mode=mode,m=m,d=d,pattern=pattern,**ref.assess(m,d,pattern,row,True)))
                if not pattern:expected.setdefault((m,d),row['result']);assert expected[m,d]==row['result']
        print(f'Verified all cells {m}x{d}, pattern {pattern}',flush=True)
    def invoke(cell,count,rounds,profile=False):
        version,mode,m,d=cell
        env=dict(os.environ,KALANDRA_GPU_PROFILE='1') if profile else None
        rows=p.observations(p.run([build/version,bundle,m,d,count,rounds,mode,0,0],env),expected[m,d],count,rounds)
        assert all(r['submissions']==r['completions'] and r['submissions']%count==0 for r in rows)
        return rows
    cells=[[v,mode,m,d] for v in ('baseline','current') for mode in range(3) for m,d in CASES]
    if args.profile_only:
        result['profiles']=[dict(cell=cell,process=process,iterations=3,samples=invoke(cell,3,3,True)) for process in range(3) for cell in (cells if process%2==0 else cells[::-1])]
    else:
        result['paired']=p.measure_paired(cells,invoke,lambda c:expected[tuple(c[2:])],blocks=args.blocks,rounds=3,target_ms=args.target_ms,adaptive=True,max_iterations=1000)
    assert {str(f):p.sha(f) for f in sources+[compiler,task,metal,baseline/'metal.a',*shaders]}==signature
    assert all(p.sha(f)==h for f,h in saved['hashes'].items())
    result.update(contract='Same complete residual FFN, source-generated frozen shader bundle, driver and task runtime. Only the Metal runtime archive changes. Mode 0 resident, 1 activation upload with resident weights, 2 all uploads. Fresh managed tasks/captures/intermediates/completion/scalar observation/cleanup included; shader loading, initialization and initial resident uploads excluded. Every shape full-cell bit checked with independent float32 tree oracle; every warmup/timed checksum checked. Profiles are separate, instrumented diagnostics, not acceptance times. No clocks/affinity/desktop isolation.',source_snapshots={p.key(f):f.read_text() for f in sources})
    p.evidence(args.output,result,sources+[Path(saved['driver'])],list(map(Path,saved['hashes']))+[metal,baseline/'metal.a',task,*shaders],compiler)

if __name__=='__main__':main()
