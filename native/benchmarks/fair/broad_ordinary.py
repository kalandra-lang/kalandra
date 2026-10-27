"""Broader ordinary CPU algorithms; strict binary64 and integer-bit checksum oracles."""
import argparse,decimal,functools,json,math,os,struct
from pathlib import Path
import protocol as p
PROGRAMS=['affine','polynomial','euclid']+[f'array{n}' for n in (8,16,32,64,128)]
@functools.cache
def oracle(program,seed,rounds):
    if program.startswith('array'):
        n=int(program[5:]);values=list(range(seed,seed+n))
        for _ in range(rounds):values=[(x*13+7)%1000003 for x in values]
        return float(sum(values))
    if program=='euclid':
        x=seed
        for _ in range(rounds):x=math.gcd(x*13+7,seed*7+11)+seed
        return float(x)
    # Independent Decimal operations, rounded explicitly to binary64 after each
    # source multiply/add. Dyadic constants exactly match all source literals.
    D=decimal.Decimal
    with decimal.localcontext() as ctx:
        ctx.prec=200
        x,offset,scale=(seed*.25,seed*.0625,.5) if program=='affine' else (seed*.015625,seed*.00390625,1.015625)
        for _ in range(rounds):
            product=float(D.from_float(x)*D.from_float(scale))
            x=float(D.from_float(product)+D.from_float(offset))
    return x

def bits(value):return struct.unpack('<Q',struct.pack('<d',value))[0]
def observations(output,expected,count,rounds):
    rows=[json.loads(x) for x in output.splitlines()]
    assert [r['round'] for r in rows]==list(range(-2,rounds))
    for row in rows:
        assert math.isfinite(row['ns']) and row['ns']>=0
        assert row['checksum']==(bits(expected)*count)&((1<<64)-1),(row,expected,count)
    return rows

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',type=Path);parser.add_argument('--reuse-build',action='store_true');parser.add_argument('--build-only',action='store_true')
    a=parser.parse_args();assert not a.output.exists()
    src=Path(__file__).resolve().parent;root=p.ROOT;build=a.build_dir.resolve();build.mkdir(parents=True,exist_ok=True)
    compiler=a.compiler.resolve() if a.compiler else root/'build/native/native/kalandra';runtime=compiler.parent/'libkalandra-task-core-optimized.a'
    sources=[Path(__file__).resolve(),src/'BroadScalar.k',src/'BroadDriver.cpp',src/'BroadReference.cpp',src/'broad_reference.rs',src/'protocol.py',root/'native/runtime/OrdinaryFault.h',root/'native/runtime/Runtime.h']
    signature={p.key(f):p.sha(f) for f in [*sources,compiler,runtime]};manifest=build/'manifest.json'
    if a.reuse_build:
        saved=json.loads(manifest.read_text());assert saved['signature']==signature
        assert all(p.sha(f)==h for f,h in saved['hashes'].items());p.COMMANDS.extend(saved['commands'])
    else:
        abi={};flags=['-std=c++23','-O2','-ffp-contract=off','-Wno-override-module','-pthread']
        for name in PROGRAMS:
            folder=build/name;folder.mkdir(exist_ok=True);source=folder/'source.k';rust=folder/'reference.rs';ir=folder/'source.ll'
            if name.startswith('array'):
                width=int(name[5:]);values=','.join('seed' if i==0 else f'seed+{i}' for i in range(width));total='+'.join(f'values[{i}]' for i in range(width))
                source.write_text(f'fn total(values:[int],index:int,acc:int)->float64:\n  if index == {width}:\n    float64(acc)\n  else:\n    total(values,index+1,acc+values[index])\nfn iterate(values:[int],rounds:int)->float64:\n  if rounds == 0:\n    total(values,0,0)\n  else:\n    iterate(map(values,fn(value:int):(value*13+7)%1000003),rounds-1)\nfn value(seed:int,rounds:int)->float64:\n  iterate([{values}],rounds)\nfn main()->float64:\n  value(17,16)\n')
                entry='value';defines=[f'-DARRAY_WIDTH={width}',f'-DARRAY_VALUES={values}'];cfg='array';rust.write_text((src/'broad_reference.rs').read_text().replace('ARRAY_VALUES',values))
            else:
                source.write_text((src/'BroadScalar.k').read_text());entry=name;defines=['-D'+name.upper()];cfg=name;rust.write_text((src/'broad_reference.rs').read_text())
            p.run([compiler,source,'--test-entry',entry,'--emit-llvm',ir,'-O2','-o',folder/'standalone'])
            direct='_mlir_ciface_kalandra_test_entry_ordinary_v1(' in ir.read_text();abi[name]='ordinary-v1' if direct else 'checked-v4'
            p.run([p.CLANG,*flags,'-DORDINARY_ENTRY' if direct else '-DCHECKED_ENTRY',src/'BroadDriver.cpp',ir,runtime,'-o',folder/'kalandra'])
            p.run([p.CLANG,*flags,*defines,'-c',src/'BroadReference.cpp','-o',folder/'cpp.o'])
            p.run([p.CLANG,*flags,src/'BroadDriver.cpp',folder/'cpp.o','-o',folder/'cpp'])
            p.run([p.RUST,'--edition=2021','--crate-type=staticlib','--cfg',cfg,'-C','opt-level=2','-C','lto=off',rust,'-o',folder/'rust.a'])
            p.run([p.CLANG,*flags,src/'BroadDriver.cpp',folder/'rust.a','-o',folder/'rust'])
        files=[f for f in build.rglob('*') if f.is_file() and f!=manifest]
        saved=dict(compiler=str(compiler),runtime=str(runtime),entry_abi=abi,signature=signature,hashes={str(f):p.sha(f) for f in files},commands=p.COMMANDS)
        manifest.write_text(json.dumps(saved,indent=2)+'\n')
    if a.build_only:return
    def invoke(cell,count,rounds,seed=17):
        lang,name,size=cell;expected=oracle(name,seed,size)
        return observations(p.run([build/name/lang,seed,size,count,rounds,repr(expected)]),expected,count,rounds)
    verification=[]
    for name in PROGRAMS:
        for lang in ('kalandra','cpp','rust'):
            for seed in (1,17,31):
                for size in (0,1,2,16,257):verification.append(dict(cell=[lang,name,size],seed=seed,samples=invoke([lang,name,size],1,1,seed)))
    cells=[[lang,name,size] for lang in ('kalandra','cpp','rust') for name in PROGRAMS for size in (0,1,16,256)]
    result=p.measure(cells,invoke,lambda c:oracle(c[1],17,c[2]),target_ms=10,adaptive=True,max_iterations=20000000)
    result.update(entry_abi=saved['entry_abi'],verification=verification,execution_contract='ordinary',contract='Identical source-level algorithms, separate per-program entry points, immutable fresh vector maps, strict binary64 multiply then add without FMA or reassociation. Ordinary Kalandra uses its proven companion when emitted, fresh checked unbound Invocation otherwise. No forced allocation or inlining controls for references. Every returned bit and wrapping integer checksum validated; independent Decimal per-operation binary64 and Python integer/GCD oracles. No build/test/profiler overlap during measurements.',source_snapshots={p.key(f):f.read_text() for f in sources})
    assert signature=={p.key(f):p.sha(f) for f in [*sources,compiler,runtime]}
    assert all(p.sha(f)==h for f,h in saved['hashes'].items())
    p.evidence(a.output,result,sources,[compiler,runtime,*map(Path,saved['hashes'])],compiler)
if __name__=='__main__':main()
