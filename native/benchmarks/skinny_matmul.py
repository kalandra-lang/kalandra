"""Strict skinny-matrix candidates: contraction parallelism, no packed inputs."""
import argparse
import json
import math
import os
from pathlib import Path
import random
import statistics
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent/'fair'))
import protocol as p

VARIANTS = [(c, w, leaf) for c, w, leaf in
            [(8,8,64),(16,8,64),(32,8,64),(32,4,64),(16,16,32),(32,8,32)]]
SHAPES = [(m,4096,4096) for m in (1,2,4,8)] + [(1,2048,2048),(1,8192,2048),
          (1,576,1027),(3,1984,2049),(8,4032,1025)]
HOLDOUT = [(1,512,256),(2,512,256),(1,544,257),(2,576,1027),(1,1984,1536),
           (2,2048,8192),(1,4096,8192),(2,4096,2049),(1,8192,4096),(2,8192,2048),
           (1,11008,1024),(2,16384,1024),(1,16384,4097),(1,512,16384),(2,16352,1025)]

def candidate(c, w, leaf, name):
    # Explicit adjacent subtrees; no dot(), simd_sum(), FMA or reassociation.
    products = '\n'.join(f'float p{i}=a.values[row*k+first+{i}]*b.values[(first+{i})*n+col];'
                         for i in range(leaf))
    nodes = [f'p{i}' for i in range(leaf)]
    while len(nodes)>1:
        nodes = [f'({nodes[i]}+{nodes[i+1]})' for i in range(0,len(nodes),2)]
    mapping = f'row=group%max(m,1u),col=(group/max(m,1u))*{c}+lane%{c}' if name.startswith('skinnycol') else f'row=group/tiles,col=(group%tiles)*{c}+lane%{c}'
    return f'''kernel void {name}(
 device KalandraHardwareMatrix& a [[buffer(0)]],
 device KalandraHardwareMatrix& b [[buffer(1)]],
 device KalandraHardwareMatrix& output [[buffer(2)]],
 device KalandraHardwareShape& parameters [[buffer(3)]],
 uint lane [[thread_position_in_threadgroup]], uint group [[threadgroup_position_in_grid]]) {{
 threadgroup float partial[{c*w}];
 const uint m=parameters.values[0],k=parameters.values[1],n=parameters.values[2];
 const uint tiles=n/{c}+uint(n%{c}!=0),{mapping};
 const uint worker=lane/{c},blocks=k/{leaf},full=blocks/{w};
 float stack[9];float tail=0.0f;
 for(uint panel=0;panel<full+uint(blocks%{w}!=0);++panel) {{
   const uint block=panel*{w}+worker,first=block*{leaf};
   if(row<m && col<n && block<blocks) {{
     {products}
     partial[lane]={nodes[0]};
   }}
   threadgroup_barrier(mem_flags::mem_threadgroup);
   if(worker==0 && row<m && col<n) {{
     float roots[{w}];uint count=min({w}u,blocks-panel*{w});
     for(uint i=0;i<count;++i) roots[i]=partial[i*{c}+lane];
     while(count>1) {{
       for(uint i=0;i<count/2;++i) roots[i]=roots[i*2]+roots[i*2+1];
       if(count&1) roots[count/2]=roots[count-1];count=(count+1)/2;
     }}
     float value=roots[0];
     if(panel<full) {{uint occupied=panel,level=0;
       while(occupied&1u) {{value=stack[level]+value;++level;occupied>>=1;}}
       stack[level]=value;
     }} else tail=value;
   }}
   threadgroup_barrier(mem_flags::mem_threadgroup);
 }}
 if(worker==0 && row<m && col<n) {{
   float value=tail;bool used=blocks%{w}!=0;
   for(uint level=0,occupied=full;occupied;++level,occupied>>=1) {{
     if(occupied&1u) {{value=used?stack[level]+value:stack[level];used=true;}}
   }}
   output.values[row*n+col]=value;
 }}
}}
'''

def source():
    header=(p.ROOT/'native/metal-source/HardwareShaders.h').read_text()
    original=header.split('hardwareMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    registers=header.split('hardwareRegisterMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    return '#include <metal_stdlib>\nusing namespace metal;\n'+original+registers+''.join(
        candidate(c,w,leaf,f'skinny_{c}_{w}_{leaf}') for c,w,leaf in VARIANTS)+''.join(
        candidate(32,w,leaf,f'skinnycol_32_{w}_{leaf}') for w,leaf in [(4,64),(8,32)])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--confirm',nargs='+')
    parser.add_argument('--blocks',type=int,default=6)
    parser.add_argument('--suite',choices=['initial','holdout'],default='initial')
    args=parser.parse_args();assert not args.output.exists()
    shapes=SHAPES if args.suite=='initial' else HOLDOUT
    assert 'KALANDRA_GPU_PROFILE' not in os.environ
    assert all(os.getenv(k) in (None,'','0') for k in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'))
    build=p.ROOT/'benchmarks/.build/skinny-2026-10-26'/args.output.stem;build.mkdir(parents=True,exist_ok=True)
    msl=build/'skinny.metal';msl.write_text(source());lib=build/'skinny.metallib'
    lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',msl,'-o',build/'skinny.air']);p.run([*metal,build/'skinny.air','-o',lib])
    driver=Path(__file__).with_name('SkinnyMatmulStudy.mm');binary=build/'skinny'
    layout=p.ROOT/'native/metal/MatmulLayout.h'
    frozen={str(x):p.sha(x) for x in [Path(__file__),driver,layout,p.ROOT/'native/metal-source/HardwareShaders.h',msl]}
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    variants=['skinny_'+ '_'.join(map(str,x)) for x in VARIANTS]+['skinnycol_32_4_64','skinnycol_32_8_32']
    def invoke(name,shape,count):
        rows=list(map(json.loads,p.run([binary,lib,name,*shape,count,3]).splitlines()))
        assert [s['round'] for s in rows]==list(range(-2,3))
        assert all(s['cells']==shape[0]*shape[2]*2 and s['gpu_ns']>0 for s in rows)
        return rows
    records=[];calibration=[];pairs=[];rng=random.Random(20261026)
    if args.confirm:
        assert all(v in variants for v in args.confirm)
        cells=[(v,s) for v in args.confirm for s in shapes];counts={}
        for v,s in cells:
            costs=[]
            for name in ('baseline',v):
                rows=invoke(name,s,1);calibration.append(dict(variant=name,shape=s,samples=rows));costs.append(statistics.median(r['gpu_ns'] for r in rows if r['round']>=0))
            counts[v,s]=max(1,min(256,math.ceil(10e6/max(costs))))
        for block in range(args.blocks):
            order=cells.copy();rng.shuffle(order)
            for v,s in order:
                med={'baseline':[],'current':[]};count=counts[v,s]
                versions=['baseline','current','current','baseline'] if block%2==0 else ['current','baseline','baseline','current']
                for slot,version in enumerate(versions):
                    rows=invoke('baseline' if version=='baseline' else v,s,count)
                    med[version].append(statistics.median(r['gpu_ns']/(count*2) for r in rows if r['round']>=0))
                    records.append(dict(block=block,slot=slot,version=version,variant=v,shape=s,iterations=count,samples=rows))
                before=statistics.mean(med['baseline']);after=statistics.mean(med['current']);pairs.append(dict(block=block,variant=v,shape=s,baseline_gpu_ns=before,current_gpu_ns=after,time_reduction=1-after/before));print(block,v,s,round(100*(1-after/before),2),flush=True)
    else:
        for trial in range(2):
            cells=[(v,s) for v in ['baseline',*variants] for s in shapes];rng.shuffle(cells)
            for v,s in cells:
                rows=invoke(v,s,1);records.append(dict(trial=trial,variant=v,shape=s,iterations=1,samples=rows));print(trial,v,s,round(statistics.median(x['gpu_ns']/2000 for x in rows if x['round']>=0),2),flush=True)
    assert all(p.sha(path)==checksum for path,checksum in frozen.items()),'source changed during study'
    p.evidence(args.output,dict(records=records,pairs=pairs,calibration=calibration,generated_msl=msl.read_text(),generator_source=Path(__file__).read_text(),driver_source=driver.read_text(),layout_source=layout.read_text(),contract='Preallocated private unmodified row-major inputs, two distinct activation/output pairs and one shared immutable weight matrix. GPU command time per call; allocation, initial uploads, loading and verification excluded. Every round checks both complete outputs, suffix guards and both inputs. Exact dyadic screening only, not managed throughput or full arbitrary-float qualification. Baseline selects production hardware block32/block64/register/tiled policy. No packing, vendor library or relaxed arithmetic.'),[Path(__file__),driver,layout,p.ROOT/'native/metal-source/HardwareShaders.h',msl],[binary,lib],p.ROOT/'build/native/native/kalandra')

if __name__=='__main__':main()
