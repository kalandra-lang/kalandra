"""Strict matmul screening: contraction panels, ping-pong panels and explicit per-lane reuse."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

def tree(values):
    while len(values)>1:values=[f'({values[i]}+{values[i+1]})' for i in range(0,len(values),2)]
    return values[0]

def kernel(name,rows=16,cols=16,threads=256,panel=64,double=False):
    basecols=min(cols,max(8,threads//rows));baserows=threads//basecols
    cells=[(r,c) for r in range(0,rows,baserows) for c in range(0,cols,basecols)]
    assert rows%baserows==0 and cols%basecols==0 and panel in [64,128,256]
    text=f'''kernel void {name}(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],constant uint* shape [[buffer(3)]],uint lane [[thread_position_in_threadgroup]],uint group [[threadgroup_position_in_grid]]) {{
threadgroup float aa[{rows*panel*(2 if double else 1)}],bb[{cols*panel*(2 if double else 1)}];
const uint m=shape[0],k=shape[1],n=shape[2],columns=(n+{cols-1})/{cols};
const uint rb=(group/columns)*{rows},cb=(group%columns)*{cols},blocks=k/{panel};
'''
    for idx,(r,c) in enumerate(cells):
        text+=f'const uint r{idx}=rb+lane/{basecols}+{r},c{idx}=cb+lane%{basecols}+{c};\n'
        text+='float '+','.join(f's{idx}_{l}=0.0f' for l in range(7))+';\n'
    def load(first,side):
        return f'''for(uint at=lane;at<{rows*panel};at+={threads}) aa[{side}*{rows*panel}+at]=rb+at/{panel}<m?a[(rb+at/{panel})*k+{first}+at%{panel}]:0.0f;
for(uint at=lane;at<{cols*panel};at+={threads}) bb[{side}*{cols*panel}+at]=cb+at%{cols}<n?b[({first}+at/{cols})*n+cb+at%{cols}]:0.0f;
'''
    if double:text+=load('0','0')+'threadgroup_barrier(mem_flags::mem_threadgroup);\n'
    text+=f'for(uint block=0;block<blocks;++block) {{ const uint first=block*{panel};\n'
    if double:
        text+='const uint side=block&1u;\nif(block+1<blocks){\n'+load(f'(first+{panel})','(side^1u)')+'}\n'
        text+=f'threadgroup float* av=aa+side*{rows*panel};threadgroup float* bv=bb+side*{cols*panel};\n'
    else:text+=load('first','0')+'threadgroup_barrier(mem_flags::mem_threadgroup);\nthreadgroup float* av=aa;threadgroup float* bv=bb;\n'
    # Explicit shared loads across output cells; do not rely on cross-scope CSE.
    roff=sorted(set(r for r,c in cells));coff=sorted(set(c for r,c in cells))
    for j in range(panel//8):
        for i in range(8):
            inner=j*8+i
            for r in roff:text+=f'float a{j}_{i}_{r}=av[(lane/{basecols}+{r})*{panel}+{inner}];\n'
            for c in coff:text+=f'float b{j}_{i}_{c}=bv[{inner*cols}+lane%{basecols}+{c}];\n'
            for idx,(r,c) in enumerate(cells):text+=f'float p{idx}_{j}_{i}=a{j}_{i}_{r}*b{j}_{i}_{c};\n'
        for idx,_ in enumerate(cells):text+=f'float v{idx}_{j}='+tree([f'p{idx}_{j}_{i}' for i in range(8)])+';\n'
    for idx,_ in enumerate(cells):
        text+=f'float value{idx}='+tree([f'v{idx}_{j}' for j in range(panel//8)])+';\n'
        for l in range(6):text+=f'if(!(block&{1<<l}u))s{idx}_{l}=value{idx};else{{value{idx}=s{idx}_{l}+value{idx};\n'
        text+=f's{idx}_6=value{idx};'+'}'*6+'\n'
    text+='threadgroup_barrier(mem_flags::mem_threadgroup);}\n'
    for idx,_ in enumerate(cells):
        text+=f'if(r{idx}<m && c{idx}<n) {{float value=0.0f;bool used=false;\n'
        for l in range(7):text+=f'if(blocks&{1<<l}u){{value=used?s{idx}_{l}+value:s{idx}_{l};used=true;}}\n'
        text+=f'out[r{idx}*n+c{idx}]=value;}}\n'
    return text+'}\n'

def scheduled(original,name,panel=64,double=False,unroll=0):
    text=original.replace('kalandra_matmul_hardware_register16',name).replace('kalandra_hardware_eight16','eight_'+name)
    if panel!=64:
        text=text.replace('row*64+first','row*'+str(panel)+'+first')
        text=text.replace('av[1024],bv[1024]',f'av[{16*panel}],bv[{16*panel}]')
        text=text.replace('blocks=k/64',f'blocks=k/{panel}').replace('first=block*64',f'first=block*{panel}')
        text=text.replace('at<1024',f'at<{16*panel}').replace('rowBase+at/64',f'rowBase+at/{panel}').replace('first+at%64',f'first+at%{panel}')
        begin=text.index('      float r0=');end=text.index('      // Explicit binary-counter',begin)
        terms=''
        for j in range(panel//8):terms+=f'      float r{j}=eight_{name}(av,bv,lane/16,lane%16,{j*8});\n'
        terms+='      float value='+tree([f'r{j}' for j in range(panel//8)])+';\n'
        text=text[:begin]+terms+text[end:]
    if double:
        assert panel==64
        start=text.index('    for(uint at=lane;at<1024;at+=256)');end=text.index('    threadgroup_barrier(mem_flags::mem_threadgroup);',start)
        load=text[start:end]
        initial=load.replace('first+','').replace('av[at]','aa[at]').replace('bv[at]','bb[at]')
        nextload=load.replace('first+','(first+64)+').replace('av[at]','aa[(side^1u)*1024+at]').replace('bv[at]','bb[(side^1u)*1024+at]')
        text=text[:start]+f'    const uint side=block&1u;\n    threadgroup float* av=aa+side*1024;threadgroup float* bv=bb+side*1024;\n    if(block+1<blocks) {{\n'+nextload+'    }\n'+text[end+len('    threadgroup_barrier(mem_flags::mem_threadgroup);\n'):]
        text=text.replace('threadgroup float av[1024],bv[1024];','threadgroup float aa[2048],bb[2048];')
        text=text.replace('  for (uint block=0;block<blocks;++block) {',initial+'    threadgroup_barrier(mem_flags::mem_threadgroup);\n  for (uint block=0;block<blocks;++block) {')
    if unroll:text=text.replace('  for (uint block=0;block<blocks;++block)',f'  #pragma clang loop unroll_count({unroll})\n  for (uint block=0;block<blocks;++block)')
    return text

def vector_kernel(name,width=2,panel=64,fixed=False):
    rows=16;cols=16;threads=rows*cols//width;levels=8 if panel==32 else 7
    dtype='float'+str(width)
    helper=f'inline {dtype} eight_{name}(threadgroup float* a,threadgroup {dtype}* b,uint row,uint col,uint first) {{\n'
    for i in range(8):helper+=f'{dtype} p{i}=a[row*{panel}+first+{i}]*b[(first+{i})*{cols//width}+col];\n'
    helper+=f'return '+tree([f'p{i}' for i in range(8)])+';}\n'
    core=f'const uint m=shape[0],k=shape[1],n=shape[2],columns=(n+15)/16,rb=(group/columns)*16,cb=(group%columns)*16,row=rb+lane/{cols//width},col=cb+(lane%{cols//width})*{width},blocks=k/{panel};\n'
    core+=dtype+' '+','.join(f's{l}={dtype}(0.0f)' for l in range(levels))+';\n'
    core+=f'for(uint block=0;block<blocks;++block) {{const uint first=block*{panel};\n'
    core+=f'for(uint at=lane;at<{rows*panel};at+={threads}) {{const uint r=rb+at/{panel},c=cb+at%16;av[at]=r<m?a[r*k+first+at%{panel}]:0.0f;bv[at]=c<n?b[(first+at/16)*n+c]:0.0f;}}\nthreadgroup_barrier(mem_flags::mem_threadgroup);\n'
    core+='if(row<m && col<n) {\n'
    for j in range(panel//8):core+=f'{dtype} r{j}=eight_{name}(av,reinterpret_cast<threadgroup {dtype}*>(bv),lane/{cols//width},lane%{cols//width},{j*8});\n'
    core+=dtype+' value='+tree([f'r{j}' for j in range(panel//8)])+';\n'
    for l in range(levels-1):core+=f'if(!(block&{1<<l}u))s{l}=value;else{{value=s{l}+value;\n'
    core+=f's{levels-1}=value;'+'}'*(levels-1)+'}\nthreadgroup_barrier(mem_flags::mem_threadgroup); }\n'
    core+=f'if(row<m && col<n) {{{dtype} value={dtype}(0.0f);bool used=false;\n'
    for l in range(levels):core+=f'if(blocks&{1<<l}u){{value=used?s{l}+value:s{l};used=true;}}\n'
    for i in range(width):core+=f'if(col+{i}<n)out[row*n+col+{i}]=value[{i}];\n'
    core+='}\n'
    args='device const float* a,device const float* b,device float* out,constant uint* shape,uint lane,uint group'
    signature=f'kernel void {name}(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],constant uint* shape [[buffer(3)]],uint lane [[thread_position_in_threadgroup]],uint group [[threadgroup_position_in_grid]])'
    if fixed:
        return helper+f'template<uint K> inline void body_{name}('+args+',threadgroup float* av,threadgroup float* bv){'+core.replace('k=shape[1]','k=K')+'}\n'+signature+'{'+f'threadgroup float av[{rows*panel}],bv[{cols*panel}];switch(shape[1]){{'+''.join(f'case {k}:body_{name}<{k}>(a,b,out,shape,lane,group,av,bv);break;' for k in [512,1024,2048])+'}}\n'
    return helper+signature+'{'+f'threadgroup float av[{rows*panel}],bv[{cols*panel}];'+core+'}\n'

def fixed_kernel(original,name,unroll=False):
    text=original.replace('kalandra_hardware_eight16','eight_'+name)
    helper,main=text.split('kernel void kalandra_matmul_hardware_register16',1)
    signature,body=main.split('{',1);body=body.rsplit('}',1)[0]
    body=body.replace('  threadgroup float av[1024],bv[1024];','').replace('k=parameters.values[1]','k=K')
    if unroll:body=body.replace('  for (uint block=0;block<blocks;++block)', '  #pragma clang loop unroll(full)\n  for (uint block=0;block<blocks;++block)')
    args='device KalandraHardwareMatrix& a,device KalandraHardwareMatrix& b,device KalandraHardwareMatrix& output,device KalandraHardwareShape& parameters,uint lane,uint group,threadgroup float* av,threadgroup float* bv'
    return helper+f'template<uint K> inline void body_{name}('+args+'){'+body+'}\n'+'kernel void '+name+signature+'{threadgroup float av[1024],bv[1024];switch(parameters.values[1]){'+''.join(f'case {k}:body_{name}<{k}>(a,b,output,parameters,lane,group,av,bv);break;' for k in [512,1024,2048])+'}}\n'

def simd_kernel(name,simdrows,simdcols):
    assert simdrows*simdcols==32
    cols=max(16,simdcols);rows=simdrows*8//(cols//simdcols)
    helper=f'inline float eight_{name}(device const float* a,device const float* b,uint m,uint k,uint n,uint row,uint col,uint slane,uint first){{\n'
    for j in range((8+simdcols-1)//simdcols):
        index=f'(slane%{simdcols}+{j*simdcols})'
        helper+=f'float a{j}=row<m && {index}<8?a[row*k+first+{index}]:0.0f;\n'
    for j in range((8+simdrows-1)//simdrows):
        index=f'(slane/{simdcols}+{j*simdrows})'
        helper+=f'float b{j}=col<n && {index}<8?b[(first+{index})*n+col]:0.0f;\n'
    for i in range(8):
        a=f'simd_shuffle(a{i//simdcols},(slane/{simdcols})*{simdcols}+{i%simdcols})'
        b=f'simd_shuffle(b{i//simdrows},{(i%simdrows)*simdcols}+slane%{simdcols})'
        if simdrows==1:b=f'b{i}'
        helper+=f'float p{i}={a}*{b};\n'
    helper+='return '+tree([f'p{i}' for i in range(8)])+';}\n'
    body=f'''kernel void {name}(device const float* a [[buffer(0)]],device const float* b [[buffer(1)]],device float* out [[buffer(2)]],constant uint* shape [[buffer(3)]],uint lane [[thread_position_in_threadgroup]],uint group [[threadgroup_position_in_grid]]) {{
const uint m=shape[0],k=shape[1],n=shape[2],columns=(n+{cols-1})/{cols};
const uint sg=lane/32,slane=lane%32,row=(group/columns)*{rows}+(sg/{cols//simdcols})*{simdrows}+slane/{simdcols},col=(group%columns)*{cols}+(sg%{cols//simdcols})*{simdcols}+slane%{simdcols},blocks=k/64;
float s0=0,s1=0,s2=0,s3=0,s4=0,s5=0,s6=0;
for(uint block=0;block<blocks;++block) {{uint first=block*64;
'''
    for j in range(8):body+=f'float r{j}=eight_{name}(a,b,m,k,n,row,col,slane,first+{j*8});\n'
    body+='float value='+tree([f'r{j}' for j in range(8)])+';\n'
    for l in range(6):body+=f'if(!(block&{1<<l}u))s{l}=value;else{{value=s{l}+value;\n'
    body+='s6=value;'+'}'*6+'}\nif(row<m && col<n){float value=0;bool used=false;\n'
    for l in range(7):body+=f'if(blocks&{1<<l}u){{value=used?s{l}+value:s{l};used=true;}}\n'
    return helper+body+'out[row*n+col]=value;}}\n',rows,cols

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--suite',choices=['initial','scheduled','vector','simd'],default='initial');a=parser.parse_args();assert not a.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','KALANDRA_GPU_PROFILE','DYLD_INSERT_LIBRARIES'):assert os.getenv(key) in (None,'','0'),key
    build=p.ROOT/'benchmarks/.build/microkernel-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True)
    header=p.ROOT/'native/metal-source/HardwareShaders.h'
    code='#include <metal_stdlib>\nusing namespace metal;\n'
    code+=header.read_text().split('hardwareMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    code+=header.read_text().split('hardwareRegisterMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    variants=[('kalandra_matmul_hardware_register16',16,16,256,64,False)]
    if a.suite=='initial':
        specs=[(16,16,256,128,False),(16,16,256,256,False),(16,16,256,64,True),(16,16,256,128,True),(16,16,128,64,False),(32,16,256,64,False),(16,32,256,64,False),(32,32,256,64,False),(32,16,256,64,True)]
        for rows,cols,threads,panel,double in specs:
            name=f'micro_{rows}_{cols}_{threads}_{panel}_{int(double)}';code+=kernel(name,rows,cols,threads,panel,double);variants.append((name,rows,cols,threads,panel,double))
    elif a.suite=='scheduled':
        original=header.read_text().split('hardwareRegisterMatmulShader = R"MSL(')[1].split(')MSL";')[0]
        for panel,double,unroll in [(64,False,2),(64,False,4),(64,False,8),(64,False,16),(128,False,0),(256,False,0),(64,True,0),(64,True,2),(128,False,2)]:
            name=f'scheduled_{panel}_{int(double)}_{unroll}'
            code+=scheduled(original,name,panel,double,unroll);variants.append((name,16,16,256,panel,double))
    elif a.suite=='vector':
        original=header.read_text().split('hardwareRegisterMatmulShader = R"MSL(')[1].split(')MSL";')[0]
        for width,panel,fixed in [(2,64,False),(2,32,False),(4,64,False),(2,64,True),(2,32,True)]:
            name=f'vector_{width}_{panel}_{int(fixed)}';code+=vector_kernel(name,width,panel,fixed);variants.append((name,16,16,256//width,panel,False))
        for unroll in [False,True]:
            name=f'fixed_{int(unroll)}';code+=fixed_kernel(original,name,unroll);variants.append((name,16,16,256,64,False))
    else:
        for sr,sc in [(1,32),(2,16),(4,8),(8,4)]:
            name=f'simd_{sr}_{sc}';text,rows,cols=simd_kernel(name,sr,sc);code+=text;variants.append((name,rows,cols,256,64,False))
    source=build/'candidates.metal';source.write_text(code)
    lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',source,'-o',build/'candidates.air'])
    p.run([*metal,build/'candidates.air','-o',build/'candidates.metallib'])
    driver=Path(__file__).with_name('MatrixKernelStudy.mm');binary=build/'study'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
    records=[];rng=random.Random(20261025)
    for trial in range(3):
        cells=[(v,n) for v in variants for n in (512,1024,2048)];rng.shuffle(cells)
        for (name,rows,cols,threads,panel,double),n in cells:
            output=p.run([binary,build/'candidates.metallib',name,rows,cols,threads,n,n,n,5])
            samples=list(map(json.loads,output.splitlines()));assert [x['round'] for x in samples]==list(range(-2,5))
            records.append(dict(trial=trial,variant=name,n=n,samples=samples))
            print(trial,name,n,statistics.median(x['gpu_ns']/1e6 for x in samples if x['round']>=0),flush=True)
    summary=[]
    for name,*_ in variants:
        for n in (512,1024,2048):
            medians=[statistics.median(x['gpu_ns'] for x in record['samples'] if x['round']>=0) for record in records if record['variant']==name and record['n']==n]
            summary.append(dict(variant=name,n=n,gpu_ns=statistics.median(medians),process_medians_ns=medians))
    p.evidence(a.output,dict(records=records,summary=summary,variants=variants,generator_source=Path(__file__).read_text(),driver_source=driver.read_text(),generated_msl=code,contract='Isolated preallocated strict GPU command screening, not managed pipeline throughput. Shared input/private output buffers; independent dyadic every-cell checks and poison/suffix guard each round outside GPU timing. Panel candidates require K divisible by their panel size and K<=4096. Three randomized processes, two warmups, five measured rounds. All candidates and samples retained.'),[Path(__file__),driver,header,source],[binary,build/'candidates.metallib'],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
