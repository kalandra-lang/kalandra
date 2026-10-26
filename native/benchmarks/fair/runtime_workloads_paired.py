"""Paired baseline/candidate array, closure and complete task-scope measurements."""
import argparse, json, os
from pathlib import Path
import protocol as p
from runtime_workloads import oracle

def artifacts(directory):
    manifest=json.loads((directory/'manifest.json').read_text())
    files=[directory/Path(name).name for name in manifest['hashes']]
    for original,digest in manifest['hashes'].items():
        assert p.sha(directory/Path(original).name)==digest,original
    return manifest,files

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--current',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--blocks',type=int,default=7)
    parser.add_argument('--target-ms',type=float,default=20)
    args=parser.parse_args()
    assert not args.output.exists() and args.blocks>=3 and args.target_ms>0
    directories=dict(baseline=args.baseline.resolve(),current=args.current.resolve())
    manifests={};files=[]
    for version,directory in directories.items():
        manifests[version],items=artifacts(directory)
        files.extend(items)
    contracts={m.get('execution_contract','cancellable') for m in manifests.values()}
    assert len(contracts)==1, 'Compare matching execution contracts'
    compiler=p.ROOT/'build/native/native/kalandra'
    for version,manifest in manifests.items():
        legacy=directories[version].parent
        defaults=(legacy/'kalandra',legacy/'runtime.a') if version=='baseline' else (
            compiler,compiler.parent/'libkalandra-task-core-optimized.a')
        for label,path,oldkey in zip(('compiler','runtime'),defaults,
                ('build/native/native/kalandra','build/native/native/libkalandra-task-core-optimized.a')):
            selected=Path(manifest.get(label,path))
            key=p.key(selected) if label in manifest else oldkey
            assert p.sha(selected)==manifest['signature'][key]
            files.append(selected)
    source_cells=[(name,kind,size) for kind,name in enumerate(('arrays','closure'))
                  for size in ((0,16,64,256) if kind==0 else (0,16,256,4096))]
    task_cells=[(mode,1,size) for mode in ('direct','scheduled-serial','scheduled')
                for size in (0,16,256)]
    def measure(groups):
        cells=[[version,*group] for group in groups for version in directories]
        def invoke(cell,count,rounds):
            version,name,kind,size=cell
            mode='scheduled' if name=='scheduled-serial' else name
            binary=directories[version]/(f'{mode}-{size}' if mode in ('direct','scheduled') else name)
            env=dict(os.environ,KALANDRA_WORKERS='1' if name=='scheduled-serial' else '8')
            return p.observations(p.run([binary,kind,17,size,count,rounds,oracle(kind,17,size)],env),
                                  oracle(kind,17,size),count,rounds)
        return p.measure_paired(cells,invoke,lambda c:oracle(c[2],17,c[3]),
                                blocks=args.blocks,target_ms=args.target_ms)
    result=dict(execution_contract=next(iter(contracts)),source=measure(source_cells),task_scope=measure(task_cells),
                manifests=manifests,
                contract='Identical frozen harness/inputs; every invocation and warmup checks an exact integer oracle. '
                         'Adjacent ABBA/BAAB processes, identical iterations per pair. Zero rounds measure fixed cost. '
                         'Task measurements include fresh scope/pool/monitor construction and settlement, with 1 or 8 '
                         'workers; they are not steady-state scheduler timings or cross-language comparisons. '
                         'No profiler/build/test overlap; no CPU affinity/frequency/desktop isolation.')
    for directory in directories.values(): artifacts(directory)
    sources=[Path(__file__),Path(__file__).with_name('runtime_workloads.py'),
             Path(__file__).with_name('protocol.py'),Path(__file__).with_name('RuntimeWorkloadDriver.cpp'),
             Path(__file__).with_name('RuntimeWorkloads.k'),p.ROOT/'native/lib/Continuation.cpp',
             p.ROOT/'native/lib/LocalStorage.h',p.ROOT/'native/lib/Backend.cpp',
             p.ROOT/'native/lib/UnboundEntry.h',
             p.ROOT/'native/runtime/Runtime.cpp',p.ROOT/'native/runtime/Runtime.h']
    result['source_snapshots']={p.key(f):f.read_text() for f in sources[:5]}
    p.evidence(args.output,result,sources,files,compiler)

if __name__=='__main__': main()
