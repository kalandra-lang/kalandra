"""Matched managed graph benchmarks with per-call numerical and cleanup checks."""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p

CASES = [(1,256,2,1), (1,512,4,1), (1,2048,2,1), (1,2048,4,1),
         (1,2048,8,1), (2,2048,4,1), (1,4096,4,1), (1,4096,8,1),
         (1,2048,2,0), (1,2048,4,0), (1,2048,8,0),
         (128,128,4,1), (512,512,2,0), (1,2048,1,0)]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-metal', type=Path, required=True)
    parser.add_argument('--blocks', type=int, default=6)
    parser.add_argument('--reuse-build', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--profile-only', action='store_true')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--baseline-batched', action='store_true')
    parser.add_argument('--target-ms', type=float, default=20)
    args = parser.parse_args()
    assert not args.output.exists()
    assert all(os.getenv(k) in (None, '', '0') for k in ('KALANDRA_GPU_PROFILE', 'MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES'))
    root = p.ROOT
    build = args.build_dir or root / 'benchmarks/.build/graph-batching-2026-10-26/paired'
    build.mkdir(parents=True, exist_ok=True)
    compiler = root / 'build/native/native/kalandra'
    task = compiler.parent / 'libkalandra-task-core-optimized.a'
    metal = compiler.parent / 'metal/libkalandra-metal-optimized.a'
    bundle = build / 'bundle'
    driver = root / 'native/benchmarks/fair/GraphBatchGpu.cpp'
    sources = [Path(__file__), driver, root/'native/benchmarks/fair/protocol.py',
               root/'native/benchmarks/GraphBatch.k', root/'native/metal/Metal.mm',
               root/'native/metal/Metal.h', root/'native/metal/MatmulLayout.h',
               root/'native/metal-source/Compiler.cpp', root/'native/metal-source/Build.cpp',
               root/'native/metal-source/MatmulEpilogue.h']
    signature = {str(f):p.sha(f) for f in sources+[compiler,task,metal,args.baseline_metal]}
    manifest = build/'manifest.json'
    if args.reuse_build:
        saved=json.loads(manifest.read_text())
        assert saved['signature']==signature
        assert all(p.sha(path)==h for path,h in saved['hashes'].items())
        p.COMMANDS.extend(saved['commands'])
    else:
        p.run([compiler,root/'native/benchmarks/GraphBatch.k','--target','metal','--arithmetic','hardware','-O2','-o',bundle])
        for version,archive in [('baseline',args.baseline_metal),('current',metal)]:
            p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-pthread',driver,archive,task,'-framework','Foundation','-framework','Metal','-o',build/version])
        artifacts=[build/'baseline',build/'current',*bundle.glob('*.metal'),*bundle.glob('*.metallib')]
        manifest.write_text(json.dumps(dict(signature=signature,hashes={str(f):p.sha(f) for f in artifacts},commands=p.COMMANDS),indent=2)+'\n')
    if args.build_only:return
    expected={}
    calibration=[]
    def invoke(cell,count,rounds,environment=None):
        version,*case=cell
        raw=p.run([build/version,bundle,*case,count,rounds],environment)
        values=[json.loads(line) for line in raw.splitlines()]
        key=tuple(case)
        expected.setdefault(key,values[0]['expected'])
        assert all(x['expected']==expected[key] for x in values)
        rows=p.observations(raw,expected[key],count,rounds)
        stages=case[2]*(2 if case[3] else 1)
        for row in rows:
            assert row['submissions']==row['completions']
            assert row['submissions']%count==0
            if version=='baseline':
                commands = ((stages+3)//4 if args.baseline_batched and case[0]*case[1]*case[1] <= 64*1024*1024 else stages)+1
                assert row['submissions']==count*commands
        if count==1:calibration.append(dict(cell=cell,iterations=count,samples=rows))
        return rows
    cells=[[v,*case] for v in ('baseline','current') for case in CASES]
    result=dict(contract='Resident dense projection chains with optional ReLU after each matrix, then strict balanced sum. Fresh task, graph captures, intermediate allocations, submissions, scalar observation and cleanup per call. Initial uploads and shader loading excluded. Same compiler, shader bundle, driver and task runtime; only Metal runtime archive differs. Complete full matrices computed on GPU; untimed CPU reference computes periodic columns using separately rounded float32 products and the identical adjacent tree. Every warmup/calibration/timed call checks scalar bits; APFloat per-cell coverage is a separate correctness gate. No CPU affinity/frequency/desktop isolation. Default 64MiB budget, raised to max(64MiB,12*N*N+32*M*N) for larger weights; no production budget change.')
    if args.profile_only:
        result['profiles']=[]
        for block in range(args.blocks):
            for case in CASES:
                for v in (('baseline','current') if block%2==0 else ('current','baseline')):
                    cell=[v,*case]
                    result['profiles'].append(dict(cell=cell,block=block,iterations=3,samples=invoke(cell,3,3,dict(os.environ,KALANDRA_GPU_PROFILE='1'))))
    else:
        result['paired']=p.measure_paired(cells,invoke,lambda c:expected[tuple(c[1:])],blocks=args.blocks,rounds=3,target_ms=args.target_ms)
    result['calibration']=calibration
    result['baseline_policy']='four-stage batching' if args.baseline_batched else 'one stage per command'
    assert {str(f):p.sha(f) for f in sources+[compiler,task,metal,args.baseline_metal]}==signature,'source changed during timing'
    saved=json.loads(manifest.read_text())
    assert all(p.sha(path)==h for path,h in saved['hashes'].items())
    result['source_snapshots']={p.key(f):f.read_text() for f in sources}
    result['baseline_metal_sha256']=p.sha(args.baseline_metal)
    p.evidence(args.output,result,sources+list(bundle.glob('*.metal')),[build/'baseline',build/'current',task,metal,args.baseline_metal,*bundle.glob('*.metallib')],compiler)

if __name__=='__main__':main()
