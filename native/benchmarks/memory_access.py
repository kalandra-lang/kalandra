"""Controlled GPU tile-order study with unchanged loads/arithmetic and packing controls."""
import argparse,json,math,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import weight_packing as packing

SHAPES=[(512,512,512),(1024,1024,1024),(2048,2048,2048),(1536,1536,1536),(1024,4096,1024),(1024,1024,2048),(1025,576,1027),(1088,1024,1024)]
VARIANTS=['baseline','order_column','order_m4','order_m8','order_m16','order_m32','order_policy','order_dispatch','block','block_m8','both','both_m8']
def source():
    header=p.ROOT/'native/metal-source/HardwareShaders.h'
    generated=packing.shaders(header,True)
    # Recover each whole strict kernel/helper from generated sections, not a new arithmetic implementation.
    for base in ['baseline','block','both']:
        start=generated.index('inline float eight_'+base+'(')
        end=generated.index('\n}',generated.index('    output.values[row*n+col]=value;',start))+2
        original=generated[start:end]
        orders=['column','m4','m8','m16','m32','policy'] if base=='baseline' else ['m8']
        for order in orders:
            name='order_'+order if base=='baseline' else base+'_'+order
            text=original.replace('matrix_'+base,'matrix_'+name).replace('eight_'+base,'eight_'+name)
            old='const uint rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*16;'
            if order=='column':
                mapping='const uint tileRows=(m+15)/16;\n  const uint rowBase=(group%tileRows)*16, colBase=(group/tileRows)*16;'
            else:
                span=16 if order=='policy' else int(order[1:])
                mapping=f'''const uint tileRows=(m+15)/16, groupSpan={span}u*tileColumns;
  const uint firstRow=(group/groupSpan)*{span}u;
  const uint height=min({span}u,tileRows-firstRow), within=group%groupSpan;
  const uint rowBase=(firstRow+within%height)*16, colBase=(within/height)*16;'''
            if order=='policy':
                mapping='const bool grouped=m>=512 && m<=65535 && n>=512 && n<=65535 &&\n      m*n>=1048576u && k*(m+n)>=4194304u;\n  uint tileRow=group/tileColumns, tileColumn=group%tileColumns;\n  if(grouped) {\n    const uint tileRows=m/16+uint(m%16!=0), groupSpan=16u*tileColumns;\n    const uint firstRow=(group/groupSpan)*16u;\n    const uint height=min(16u,tileRows-firstRow), within=group%groupSpan;\n    tileRow=firstRow+within%height;tileColumn=within/height;\n  }\n  const uint rowBase=tileRow*16, colBase=tileColumn*16;'
            assert text.count(old)==1
            generated+=text.replace(old,mapping)+'\n'
    return generated

def main():
    a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);a.add_argument('--confirm',nargs='+');a.add_argument('--blocks',type=int,default=5);a.add_argument('--suite',choices=['screen','boundaries'],default='screen');args=a.parse_args();assert not args.output.exists()
    shapes=SHAPES if args.suite=='screen' else [(512,512,512),(1024,1024,1024),(1024,1984,1024),(1024,2048,1024),(1024,2112,1024),(1536,1984,1024),(2048,1024,2048),(2048,4096,1024),(1024,4096,2048),(4096,4096,256),(256,4096,4096),(513,4096,2049),(2049,4096,513),(1025,2048,1027),(512,4096,2048),(2048,4096,512)]
    assert all(os.getenv(k) in (None,'','0') for k in ['MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_GPU_PROFILE'])
    build=p.ROOT/'benchmarks/.build/memory-access-2026-10-26'/args.output.stem;build.mkdir(parents=True,exist_ok=True)
    msl=build/'access.metal';msl.write_text(source());lib=build/'access.metallib';lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',msl,'-o',build/'access.air']);p.run([*metal,build/'access.air','-o',lib])
    driver=Path(__file__).with_name('MemoryAccessStudy.mm');binary=build/'access'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks','-DKALANDRA_PACK_TILED',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    def invoke(name,shape,count):
        rows=list(map(json.loads,p.run([binary,lib,name,*shape,1,3,count]).splitlines()));assert [r['round'] for r in rows]==list(range(-2,3))
        assert all(r['cells']==shape[0]*shape[2] and r['gpu_ns']>0 for r in rows);return rows
    rng=random.Random(20261026);records=[];pairs=[];calibration=[]
    if args.confirm:
        assert all(v in VARIANTS and v!='baseline' for v in args.confirm)
        cells=[(v,s) for v in args.confirm for s in shapes];counts={}
        for v,s in cells:
            costs=[]
            for name in ['baseline',v]:
                rows=invoke(name,s,1);costs.append(statistics.median(r['gpu_ns'] for r in rows if r['round']>=0));calibration.append(dict(variant=name,shape=s,samples=rows))
            counts[v,s]=max(1,min(512,math.ceil(20e6/max(costs))))
        for block in range(args.blocks):
            order=cells.copy();rng.shuffle(order)
            for v,s in order:
                med={'baseline':[],'current':[]};count=counts[v,s]
                for slot,version in enumerate(['baseline','current','current','baseline'] if block%2==0 else ['current','baseline','baseline','current']):
                    rows=invoke('baseline' if version=='baseline' else v,s,count);med[version].append(statistics.median(r['gpu_ns']/count for r in rows if r['round']>=0));records.append(dict(block=block,slot=slot,version=version,variant=v,shape=s,iterations=count,samples=rows))
                before=statistics.mean(med['baseline']);after=statistics.mean(med['current']);pairs.append(dict(block=block,variant=v,shape=s,baseline_gpu_ns=before,current_gpu_ns=after,time_reduction=1-after/before));print(block,v,s,round(100*(1-after/before),2),flush=True)
    else:
        for trial in range(2):
            cells=[(v,s) for v in VARIANTS for s in shapes];rng.shuffle(cells)
            for v,s in cells:
                rows=invoke(v,s,1);records.append(dict(trial=trial,variant=v,shape=s,iterations=1,samples=rows));print(trial,v,s,round(statistics.median(r['gpu_ns']/1e6 for r in rows if r['round']>=0),3),flush=True)
    p.evidence(args.output,dict(suite=args.suite,records=records,pairs=pairs,calibration=calibration,generated_msl=msl.read_text(),generator_source=Path(__file__).read_text(),driver_source=driver.read_text(),contract='Only mapping from threadgroup ID to output tile changes. Loads, arithmetic tree and allocation lengths are unchanged within each storage family. Block/both include fresh GPU packing every iteration, R=1. Preallocated buffers; allocation/uploads/loading excluded. Every final output cell, guard, packed word/padding and original input is checked after every warmup/round. Screening uses two randomized processes; confirmation uses mirrored paired ABBA/BAAB blocks and equal 20ms-calibrated batches. Exact dyadic diagnostics, not complete managed throughput or full nonfinite/safety qualification.'),[Path(__file__),driver,Path(packing.__file__),p.ROOT/'native/metal-source/HardwareShaders.h',p.ROOT/'native/metal/MatmulLayout.h',msl],[binary,lib],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
