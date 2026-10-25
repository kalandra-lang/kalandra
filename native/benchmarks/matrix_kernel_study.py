"""Screen strict matrix tile/storage candidates; never modifies production policy."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def kernel(name,rows,cols,threads,scalar=True):
    # Each lane owns one or more cells. Aligned 64-leaf subtrees are unchanged.
    basecols=min(cols,max(8,threads//rows));baserows=threads//basecols
    outputs=[(r,c) for r in range(rows//baserows) for c in range(cols//basecols)]
    assert rows%baserows==0 and cols%basecols==0
    text=f'''kernel void {name}(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],constant uint* shape [[buffer(3)]],uint lane [[thread_position_in_threadgroup]],uint group [[threadgroup_position_in_grid]]) {{
threadgroup float av[{rows*64}],bv[{cols*64}];
const uint m=shape[0],k=shape[1],n=shape[2],columns=(n+{cols-1})/{cols};
const uint rb=(group/columns)*{rows},cb=(group%columns)*{cols};
const uint blocks=k/64;
'''
    for idx,(r,c) in enumerate(outputs):
        text+=f'const uint r{idx}=rb+lane/{basecols}+{r*baserows},c{idx}=cb+lane%{basecols}+{c*basecols};\n'
        text+=(','.join(f'float s{idx}_{l}=0' for l in range(7)).replace(',float ', ',')+';\n') if scalar else f'float stack{idx}[7];\n'
    text+='for(uint block=0;block<blocks;++block) { const uint first=block*64;\n'
    text+=f'for(uint at=lane;at<{rows*64};at+={threads}) av[at]=rb+at/64<m?a[(rb+at/64)*k+first+at%64]:0.0f;\n'
    text+=f'for(uint at=lane;at<{cols*64};at+={threads}) bv[at]=cb+at%{cols}<n?b[(first+at/{cols})*n+cb+at%{cols}]:0.0f;\nthreadgroup_barrier(mem_flags::mem_threadgroup);\n'
    for idx,(r,c) in enumerate(outputs):
        text+=f'if(r{idx}<m && c{idx}<n) {{\n'
        roots=[]
        for j in range(8):
            for i in range(8):text+=f'float p{j}_{i}=av[(lane/{basecols}+{r*baserows})*64+{j*8+i}]*bv[{(j*8+i)*cols}+lane%{basecols}+{c*basecols}];\n'
            text+=f'float v{j}=((p{j}_0+p{j}_1)+(p{j}_2+p{j}_3))+((p{j}_4+p{j}_5)+(p{j}_6+p{j}_7));\n';roots.append(f'v{j}')
        text+='float value=((v0+v1)+(v2+v3))+((v4+v5)+(v6+v7));\n'
        if scalar:
            for l in range(6):text+=f'if(!(block&{1<<l}u)) s{idx}_{l}=value; else {{ value=s{idx}_{l}+value;\n'
            text+=f's{idx}_6=value;'+('}'*6)+'\n'
        else:text+=f'uint occupied=block,level=0;while(occupied&1){{value=stack{idx}[level]+value;++level;occupied>>=1;}}stack{idx}[level]=value;\n'
        text+='}\n'
    text+='threadgroup_barrier(mem_flags::mem_threadgroup);}\n'
    for idx,_ in enumerate(outputs):
        text+=f'if(r{idx}<m && c{idx}<n) {{ float value=0.0f;bool used=false;\n'
        for l in range(7):
            root=f's{idx}_{l}' if scalar else f'stack{idx}[{l}]'
            text+=f'if(blocks&{1<<l}u){{value=used?{root}+value:{root};used=true;}}\n'
        text+=f'out[r{idx}*n+c{idx}]=value;}}\n'
    return text+'}\n'

def cooperative(name,rows,cols,parts):
    threads=rows*cols*parts;leaves=64//parts
    text=f'''kernel void {name}(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],constant uint* shape [[buffer(3)]],uint lane [[thread_position_in_threadgroup]],uint group [[threadgroup_position_in_grid]]) {{
threadgroup float av[{rows*64}],bv[{cols*64}];
const uint m=shape[0],k=shape[1],n=shape[2],columns=(n+{cols-1})/{cols};
const uint rb=(group/columns)*{rows},cb=(group%columns)*{cols},row=lane/{cols*parts},col=(lane/{parts})%{cols},part=lane%{parts};
const uint blocks=k/64;float s0=0,s1=0,s2=0,s3=0,s4=0,s5=0,s6=0;
for(uint block=0;block<blocks;++block) {{ const uint first=block*64;
for(uint at=lane;at<{rows*64};at+={threads}) av[at]=rb+at/64<m?a[(rb+at/64)*k+first+at%64]:0.0f;
for(uint at=lane;at<{cols*64};at+={threads}) bv[at]=cb+at%{cols}<n?b[(first+at/{cols})*n+cb+at%{cols}]:0.0f;
threadgroup_barrier(mem_flags::mem_threadgroup);
'''
    values=[]
    for i in range(leaves):
        text+=f'float p{i}=av[row*64+part*{leaves}+{i}]*bv[(part*{leaves}+{i})*{cols}+col];\n';values.append(f'p{i}')
    while len(values)>1:values=[f'({values[i]}+{values[i+1]})' for i in range(0,len(values),2)]
    text+=f'float value={values[0]};\n'
    step=1
    while step<parts:
        text+=f'value=value+simd_shuffle_down(value,{step}u);\n';step*=2
    text+='if(part==0 && rb+row<m && cb+col<n) {\n'
    for level in range(6):text+=f'if(!(block&{1<<level}u))s{level}=value;else{{value=s{level}+value;\n'
    text+='s6=value;'+'}'*6+'}\nthreadgroup_barrier(mem_flags::mem_threadgroup);}\n'
    text+='if(part==0 && rb+row<m && cb+col<n){float value=0;bool used=false;\n'
    for level in range(7):text+=f'if(blocks&{1<<level}u){{value=used?s{level}+value:s{level};used=true;}}\n'
    return text+'out[(rb+row)*n+cb+col]=value;}}\n'

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--suite',choices=['initial','extended'],default='initial');a=parser.parse_args();assert not a.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','KALANDRA_GPU_PROFILE','DYLD_INSERT_LIBRARIES'):assert os.getenv(key) in (None,'','0'),key
    build=p.ROOT/'benchmarks/.build/large-matrix-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True)
    header=p.ROOT/'native/metal-source/HardwareShaders.h';original=header.read_text().split('hardwareMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    variants=[('baseline',16,8,128)]
    code='#include <metal_stdlib>\nusing namespace metal;\n'+original
    code+=original[original.index('// A 16x8'):].replace('kalandra_matmul_hardware_block64','stack7').replace('float stack[27]','float stack[7]').replace('kalandra_hardware_eight64','eight_short')
    variants.append(('stack7',16,8,128))
    for rows,cols,threads in ((8,8,64),(16,8,128),(16,16,128),(16,16,256),(32,8,128),(32,16,128),(32,16,256)):
        name=f'register_{rows}_{cols}_{threads}';code+=kernel(name,rows,cols,threads);variants.append((name,rows,cols,threads))
    if a.suite=='extended':
        variants=[('baseline',16,8,128),('register_16_16_256',16,16,256)]
        for rows,cols,threads in ((16,32,256),(32,32,256),(32,32,512)):
            name=f'wide_{rows}_{cols}_{threads}';code+=kernel(name,rows,cols,threads);variants.append((name,rows,cols,threads))
        for rows,cols,parts in ((4,4,8),(8,8,4),(16,8,2),(16,8,4),(16,16,2)):
            name=f'cooperative_{rows}_{cols}_{parts}';code+=cooperative(name,rows,cols,parts);variants.append((name,rows,cols,rows*cols*parts))
    source=build/'candidates.metal';source.write_text(code)
    lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',source,'-o',build/'candidates.air'])
    p.run([*metal,build/'candidates.air','-o',build/'candidates.metallib'])
    driver=Path(__file__).with_name('MatrixKernelStudy.mm');binary=build/'study'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    records=[];rng=random.Random(20261025)
    for trial in range(3):
        cells=[(v,n) for v in variants for n in (512,1024,2048)];rng.shuffle(cells)
        for (name,rows,cols,threads),n in cells:
            entry='kalandra_matmul_hardware_block64' if name=='baseline' else name
            output=p.run([binary,build/'candidates.metallib',entry,rows,cols,threads,n,n,n,5])
            samples=list(map(json.loads,output.splitlines()));assert [x['round'] for x in samples]==list(range(-2,5))
            records.append(dict(trial=trial,variant=name,n=n,samples=samples))
            print(trial,name,n,statistics.median(x['gpu_ns']/1e6 for x in samples if x['round']>=0),flush=True)
    summary=[]
    for name,*_ in variants:
        for n in (512,1024,2048):
            medians=[statistics.median(x['gpu_ns'] for x in record['samples'] if x['round']>=0) for record in records if record['variant']==name and record['n']==n]
            summary.append(dict(variant=name,n=n,gpu_ns=statistics.median(medians),process_medians_ns=medians))
    p.evidence(a.output,dict(records=records,summary=summary,variants=variants,generator_source=Path(__file__).read_text(),driver_source=driver.read_text(),generated_msl=code,contract='Isolated screening only. Strict 64-leaf product/tree arithmetic. Candidate scalar-root storage is bounded to K<=4096. Preallocated buffers, GPU timestamps, readback and independent dyadic per-cell checks outside timing; poisoned output/suffix guard each round. Not final language performance.'),[Path(__file__),driver,header,source],[binary,build/'candidates.metallib'],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
