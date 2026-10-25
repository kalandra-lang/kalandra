"""Screen strict GPU matmul input layouts, including GPU construction and repeated-weight use."""
import argparse,json,os,random,statistics,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p

LAYOUTS=['baseline','panel','block','column','tpanel','swizzle','both','blockpad','panelpad']
def shaders(header,tiled=False):
    original=header.read_text().split('hardwareRegisterMatmulShader = R"MSL(')[1].split(')MSL";')[0]
    code='#include <metal_stdlib>\nusing namespace metal;\nstruct KalandraHardwareMatrix {float values[1];};struct KalandraHardwareShape {uint values[3];};\n'
    for name in (LAYOUTS if tiled else LAYOUTS[:7]):
        text=original.replace('kalandra_matmul_hardware_register16','matrix_'+name).replace('kalandra_hardware_eight16','eight_'+name)
        if name in ['panel','both']:
            text=text.replace('c<n ? b.values[(first+at/16)*n+c] : 0.0f','b.values[((colBase/16)*(k/64)+block)*1024+at]')
        elif name in ['blockpad','panelpad']:
            offset='block*(((n+15)/16)*1024+32)+(colBase/16)*1024+at' if name=='blockpad' else '(colBase/16)*((k/64)*1024+32)+block*1024+at'
            text=text.replace('c<n ? b.values[(first+at/16)*n+c] : 0.0f','b.values['+offset+']')
        elif name=='block':
            text=text.replace('c<n ? b.values[(first+at/16)*n+c] : 0.0f','b.values[(block*((n+15)/16)+colBase/16)*1024+at]')
        elif name=='swizzle':
            text=text.replace('c<n ? b.values[(first+at/16)*n+c] : 0.0f','b.values[((colBase/16)*(k/64)+block)*1024+at]')
            for i in range(8):text=text.replace(f'b[(first+{i})*16+col]',f'b[(first+{i})*16+(col^((first+{i})&15))]')
        elif name in ['column','tpanel']:
            text=text.replace('bv[1024]','bv[1088]')
            for i in range(8):text=text.replace(f'b[(first+{i})*16+col]',f'b[(first+{i})*17+col]')
            source='b.values[(colBase+at/64)*k+first+at%64]' if name=='column' else 'b.values[((colBase/16)*(k/64)+block)*1024+at]'
            text=text.replace('bv[at]=c<n ? b.values[(first+at/16)*n+c] : 0.0f',f'bv[(at%64)*17+at/64]={source}')
        if name=='both':text=text.replace('r<m ? a.values[r*k+first+at%64] : 0.0f','a.values[((rowBase/16)*(k/64)+block)*1024+at]')
        code+=text
    if tiled:return code+tiled_packers()
    for name in LAYOUTS[1:7]+['left']:
        if name in ['panel','both','swizzle']:
            mapping='uint tile=index/1024,i=(tile%(k/64))*64+(index%1024)/16,c=(tile/(k/64))*16+index%16;'
            if name=='swizzle':mapping+='c=(c&~15u)|((c&15u)^(i&15u));'
        elif name=='block':mapping='uint tile=index/1024,i=(tile/((n+15)/16))*64+(index%1024)/16,c=(tile%((n+15)/16))*16+index%16;'
        elif name=='column':mapping='uint c=index/k,i=index%k;'
        elif name=='tpanel':mapping='uint tile=index/1024,c=(tile/(k/64))*16+(index%1024)/64,i=(tile%(k/64))*64+index%64;'
        else:mapping='uint tile=index/1024,r=(tile/(k/64))*16+(index%1024)/64,i=(tile%(k/64))*64+index%64;'
        count='((m+15)/16)*16*k' if name=='left' else '((n+15)/16)*16*k'
        value='r<m?input[r*k+i]:0u' if name=='left' else 'c<n?input[i*n+c]:0u'
        code+=f'''kernel void pack_{name}(device const uint* input [[buffer(0)]],device uint* output [[buffer(1)]],constant uint* shape [[buffer(2)]],uint index [[thread_position_in_grid]]) {{uint m=shape[0],k=shape[1],n=shape[2];if(index<{count}){{{mapping}output[index]={value};}}}}\n'''
    return code

def tiled_packers():
    code=''
    for name in LAYOUTS[1:]+['left']:
        base='uint m=shape[0],k=shape[1],n=shape[2];'
        if name=='column':
            body=base+'''threadgroup uint tile[32*33];
for(uint at=lane;at<1024;at+=256){uint i=group.y*32+at/32,c=group.x*32+at%32;tile[(at/32)*33+at%32]=c<n?input[i*n+c]:0u;}
threadgroup_barrier(mem_flags::mem_threadgroup);
for(uint at=lane;at<1024;at+=256){uint c=group.x*32+at/32,i=group.y*32+at%32;if(c<((n+15)/16)*16)output[c*k+i]=tile[(at%32)*33+at/32];}
'''
        elif name=='tpanel':
            body=base+'''threadgroup uint tile[64*17];
for(uint at=lane;at<1024;at+=256){uint i=group.y*64+at/16,c=group.x*16+at%16;tile[(at/16)*17+at%16]=c<n?input[i*n+c]:0u;}
threadgroup_barrier(mem_flags::mem_threadgroup);
uint base=(group.x*(k/64)+group.y)*1024;
for(uint at=lane;at<1024;at+=256)output[base+at]=tile[(at%64)*17+at/64];
'''
        else:
            dest='(group.y*((n+15)/16)+group.x)*1024+at' if name=='block' else '(group.x*(k/64)+group.y)*1024+at'
            if name=='blockpad':dest='group.y*(((n+15)/16)*1024+32)+group.x*1024+at'
            if name=='panelpad':dest='group.x*((k/64)*1024+32)+group.y*1024+at'
            if name=='left':
                mapping='uint r=group.x*16+at/64,i=group.y*64+at%64;'
                value='r<m?input[r*k+i]:0u'
            else:
                mapping='uint i=group.y*64+at/16,c=group.x*16+at%16;'
                if name=='swizzle':mapping+='c=(c&~15u)|((c&15u)^(i&15u));'
                value='c<n?input[i*n+c]:0u'
            body=base+'for(uint at=lane;at<1024;at+=256){'+mapping+f'output[{dest}]={value};'+'}\n'
            if name=='blockpad':body+='if(group.x==0 && lane<32)output[group.y*(((n+15)/16)*1024+32)+((n+15)/16)*1024+lane]=0u;\n'
            if name=='panelpad':body+='if(group.y==0 && lane<32)output[group.x*((k/64)*1024+32)+(k/64)*1024+lane]=0u;\n'
        code+=f'kernel void pack_{name}(device const uint* input [[buffer(0)]],device uint* output [[buffer(1)]],constant uint* shape [[buffer(2)]],uint3 local [[thread_position_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]]){{uint lane=local.x;{body}}}\n'
    return code

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);parser.add_argument('--pack-method',choices=['flat','tiled'],default='flat');parser.add_argument('--layouts',nargs='+');a=parser.parse_args();assert not a.output.exists()
    for key in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','KALANDRA_GPU_PROFILE','DYLD_INSERT_LIBRARIES'):assert os.getenv(key) in (None,'','0'),key
    build=p.ROOT/'benchmarks/.build/weight-packing-2026-10-25'/a.output.stem;build.mkdir(parents=True,exist_ok=True)
    header=p.ROOT/'native/metal-source/HardwareShaders.h';source=build/'packing.metal';source.write_text(shaders(header,a.pack_method=='tiled'))
    lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
    p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',source,'-o',build/'packing.air'])
    p.run([*metal,build/'packing.air','-o',build/'packing.metallib'])
    driver=Path(__file__).with_name('WeightPackingStudy.mm');binary=build/'packing'
    p.run([p.CLANG,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fobjc-arc','-fblocks',*(['-DKALANDRA_PACK_TILED'] if a.pack_method=='tiled' else []),driver,'-framework','Foundation','-framework','Metal','-o',binary])
    layouts=a.layouts or LAYOUTS[:7];assert all(v in LAYOUTS for v in layouts) and (a.pack_method=='tiled' or all(v in LAYOUTS[:7] for v in layouts))
    records=[];rng=random.Random(20261025)
    for trial in range(3):
        cells=[(layout,n,reuse) for layout in layouts for n in [512,1024,2048] for reuse in [1,8]];rng.shuffle(cells)
        for layout,n,reuse in cells:
            samples=list(map(json.loads,p.run([binary,build/'packing.metallib',layout,n,n,n,reuse,3]).splitlines()))
            assert [r['round'] for r in samples]==list(range(-2,3))
            assert all(r['cells']==n*n*reuse and r['gpu_ns']>0 and r['ns']>0 for r in samples)
            records.append(dict(trial=trial,layout=layout,shape=[n,n,n],reuse=reuse,samples=samples))
            print(trial,layout,n,reuse,statistics.median(r['gpu_ns']/reuse/1e6 for r in samples if r['round']>=0),flush=True)
    summary=[]
    for layout in layouts:
        for n in [512,1024,2048]:
            for reuse in [1,8]:
                vals=[statistics.median(r['gpu_ns']/reuse for r in rec['samples'] if r['round']>=0) for rec in records if (rec['layout'],rec['shape'][0],rec['reuse'])==(layout,n,reuse)]
                summary.append(dict(layout=layout,n=n,reuse=reuse,gpu_ns_per_call=statistics.median(vals),process_medians_ns=vals))
    p.evidence(a.output,dict(records=records,summary=summary,layouts=layouts,pack_method=a.pack_method,generator_source=Path(__file__).read_text(),driver_source=driver.read_text(),generated_msl=source.read_text(),contract='Preallocated private input/output/packed buffers. GPU command interval includes fresh B packing once per round and, for both, A packing every call; repeated calls alternate two activation tensors, share one immutable B, and have distinct outputs. Packing is not retained across warmup/measured rounds. Host wall includes encoding/commit/wait but excludes buffer allocation, shader loading, uploads and output verification. Every output cell and guard, packed contents/padding/guard, and input immutability are checked outside timing every round. Strict dyadic screening, not full language/non-dyadic/safety qualification. R=1 includes construction for a single use; R=8 amortizes construction within the batch. Packed workspace and total preallocated GPU bytes are recorded.'),[Path(__file__),driver,header,source],[binary,build/'packing.metallib'],p.ROOT/'build/native/native/kalandra')
if __name__=='__main__':main()
