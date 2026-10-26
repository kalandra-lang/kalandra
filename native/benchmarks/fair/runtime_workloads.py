"""Equivalent array/capture algorithms plus a separate whole-scope task diagnostic."""
import argparse
import functools
import json
import os
import re
from pathlib import Path
import protocol as p

@functools.cache
def oracle(kind, seed, rounds):
    values = list(range(seed, seed+8)) if kind == 0 else [seed]
    for _ in range(rounds):
        values = [(value*13+(7 if kind == 0 else seed+7)) % 1000003 for value in values]
    return sum(values)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--reuse-build', action='store_true')
    args = parser.parse_args()
    assert not args.output.exists()
    root, src = p.ROOT, Path(__file__).resolve().parent
    build = p.BUILD/'runtime-workloads'
    build.mkdir(parents=True, exist_ok=True)
    compiler = root/'build/native/native/kalandra'
    runtime = compiler.parent/'libkalandra-task-core-optimized.a'
    source, driver = src/'RuntimeWorkloads.k', src/'RuntimeWorkloadDriver.cpp'
    flags = ['-std=c++23','-O2','-ffp-contract=off','-Wno-override-module','-pthread']
    paths = [Path(__file__), source, driver, src/'RuntimeWorkloads.cpp', src/'runtime_workloads.rs',
             src/'protocol.py', root/'native/runtime/TaskABI.cpp', compiler, runtime]
    signature = {p.key(f):p.sha(f) for f in paths}
    manifest = build/'manifest.json'
    if args.reuse_build:
        saved = json.loads(manifest.read_text())
        assert saved['signature'] == signature
        assert all(p.sha(f) == h for f,h in saved['hashes'].items())
        p.COMMANDS.extend(saved['commands'])
    else:
        p.run([p.CLANG,*flags,'-c',src/'RuntimeWorkloads.cpp','-o',build/'cpp.o'])
        p.run([p.CLANG,*flags,driver,build/'cpp.o','-o',build/'cpp'])
        p.run([p.RUST,'--edition=2021','--crate-type=staticlib','-C','opt-level=2','-C','lto=off',
               src/'runtime_workloads.rs','-o',build/'rust.a'])
        p.run([p.CLANG,*flags,driver,build/'rust.a','-o',build/'rust'])
        for name in ('arrays','closure'):
            ir = build/(name+'.ll')
            p.run([compiler,source,'--test-entry',name,'--emit-llvm',ir,'-O2','-o',build/(name+'-standalone')])
            p.run([p.CLANG,*flags,'-DKALANDRA_KERNEL',driver,ir,runtime,'-o',build/name])
        for rounds in (0,16,256):
            for mode in ('direct','scheduled'):
                name = f'{mode}-{rounds}'
                fixed = build/(name+'.k')
                scheduled = '''\nfn scheduled(seed:int,rounds:int)->float64 ! TaskError:
  try parallel:
    work=spawn closure(seed,rounds)
    try await work
''' if mode == 'scheduled' else ''
                fixed.write_text(source.read_text().split('fn main()')[0]+scheduled+'\nfn main()->float64'+
                    (' ! TaskError' if mode == 'scheduled' else '')+':\n  '+
                    ('try scheduled' if mode == 'scheduled' else 'closure')+f'(17,{rounds})\n')
                ir = build/(name+'.ll')
                p.run([compiler,fixed,'--test-entry','main','--emit-llvm',ir,'-O2','-o',build/(name+'-standalone')])
                entry = '_mlir_ciface_kalandra_'+('float64_entry' if mode == 'scheduled' else 'test_entry')
                assert re.search(r'define double @'+entry+r'\(ptr ',ir.read_text()), entry
                p.run([p.CLANG,*flags,'-DFIXED_ENTRY','-DENTRY_NAME='+entry,driver,ir,
                       root/'native/runtime/TaskABI.cpp',runtime,'-o',build/name])
        artifacts = [f for f in build.iterdir() if f.is_file() and f != manifest]
        saved = dict(signature=signature,hashes={str(f):p.sha(f) for f in artifacts},commands=p.COMMANDS)
        manifest.write_text(json.dumps(saved,indent=2)+'\n')
    if args.build_only: return
    def invoke(cell,count,rounds,seed=17):
        language,kind,size = cell
        name = 'scheduled' if language == 'scheduled-serial' else language
        binary = build/(('arrays','closure')[kind] if language == 'kalandra' else
                        f'{name}-{size}' if name in ('direct','scheduled') else name)
        env = dict(os.environ,KALANDRA_WORKERS='1' if language == 'scheduled-serial' else '8')
        return p.observations(p.run([binary,kind,seed,size,count,rounds,oracle(kind,seed,size)],env),
                              oracle(kind,seed,size),count,rounds)
    verification = []
    for lang in ('kalandra','cpp','rust'):
        for kind in range(2):
            for size in (0,1,2,16,257):
                for seed in (1,17,31):
                    verification.append(dict(cell=[lang,kind,size],seed=seed,samples=invoke([lang,kind,size],1,1,seed)))
    cells = [[lang,kind,size] for lang in ('kalandra','cpp','rust') for kind in range(2)
             for size in ((0,16,64,256) if kind == 0 else (0,16,256,4096))]
    diagnostic = [[mode,1,size] for mode in ('direct','scheduled','scheduled-serial') for size in (0,16,256)]
    for cell in diagnostic:
        verification.append(dict(cell=cell,seed=17,samples=invoke(cell,1,1)))
    result = p.measure(cells,invoke,lambda c:oracle(c[1],17,c[2]),target_ms=20)
    result['task_scope_diagnostic'] = p.measure(diagnostic,invoke,lambda c:oracle(1,17,c[2]),target_ms=20)
    result['verification'] = verification
    result['contract'] = ('Equivalent runtime-input algorithms: eight-element immutable array maps with a fresh output each pass, and a scalar-capturing closure repeatedly passed through a tail-recursive helper. Each language may optimize normally; C++/Rust use templates/generics, with no forced dynamic calls. Kalandra includes fresh invocation, cancellation, checked arithmetic, allocation/ownership and settlement. C++/Rust retain normal semantics. Identical external C++ timing/validation harness; separate objects/no LTO. Exact independent integer oracle checked every call. Task diagnostic is Kalandra-only, fixed-input complete parallel-scope creation/spawn/await/destruction versus a fixed-input synchronous entry; scheduled explicitly uses 8 workers, scheduled-serial uses the host-driven 1-worker scheduler. It is not a steady-state scheduler microbenchmark or a cross-language ranking.')
    assert {p.key(f):p.sha(f) for f in paths} == signature
    assert all(p.sha(f) == h for f,h in saved['hashes'].items())
    result['source_snapshots'] = {p.key(f):f.read_text() for f in paths[:-2]}
    p.evidence(args.output,result,paths[:-2],[compiler,runtime,*map(Path,saved['hashes'])],compiler)

if __name__ == '__main__': main()
