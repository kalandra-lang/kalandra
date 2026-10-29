"""Strict output reuse with contiguous per-warp columns and optional row reuse.

Follow-up to the rejected adjacent-column layout. Only copied experiment shaders
and drivers change. Full bits, CPU tree oracle, inputs and guards are checked.
"""
import argparse,json,os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
from output_reuse_shader import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True)
parser.add_argument('--layout',choices=('spaced','quad'),required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--blocks',type=int,default=5)
a=parser.parse_args();assert not a.output.exists()
base=p.ROOT/'benchmarks/.build/gpu-output-reuse-2026-10-29'
build=base/a.layout;build.mkdir(parents=True,exist_ok=True)
frozen=base/'screen/study.metal';driver_source=Path(__file__).with_name('OutputReuseStudy.mm')
sources=[Path(__file__),driver_source,frozen,Path(__file__).parent/'fair/output_reuse_shader.py',Path(__file__).parent/'fair/protocol.py']
signature={p.key(f.resolve()):p.sha(f) for f in sources};manifest=build/'manifest.json'
binary=build/'screen';library=build/'study.metallib'
if not manifest.exists():
 assert a.phase=='prepare'
 source=frozen.read_text();helper=function(source,'inline float2 kalandra_hardware_eight_pair32(')
 kernel=function(source,'kernel void kalandra_matmul_hardware_pair32(')
 helper=helper.replace('pair32','reuse_'+a.layout).replace('*32+col+1]','*32+col+16]')
 kernel=kernel.replace('pair32','reuse_'+a.layout).replace('colBase+(lane%16)*2','colBase+lane%16').replace('(lane%16)*2,','lane%16,').replace('col+1','col+16')
 if a.layout=='quad':
  helper='inline float4 kalandra_hardware_eight_reuse_quad(threadgroup float* a, threadgroup float* b,uint row,uint col,uint first) {\n'
  for i in range(8):
   helper+=f'float4 p{i}=float4(a[row*64+first+{i}],a[row*64+first+{i}],a[(row+16)*64+first+{i}],a[(row+16)*64+first+{i}])*float4(b[(first+{i})*32+col],b[(first+{i})*32+col+16],b[(first+{i})*32+col],b[(first+{i})*32+col+16]);\n'
  helper+='return ((p0+p1)+(p2+p3))+((p4+p5)+(p6+p7));\n}\n'
  kernel=kernel.replace('float2','float4').replace('av[1024]','av[2048]').replace('rowBase=(group/tileColumns)*16','rowBase=(group/tileColumns)*32').replace('at<1024','at<2048')
  old='if(col+16<n) output.values[row*n+col+16]=value.y;'
  assert kernel.count(old)==1
  kernel=kernel.replace(old,old+'\n    if(row+16<m) { output.values[(row+16)*n+col]=value.z; if(col+16<n) output.values[(row+16)*n+col+16]=value.w; }')
 (build/'study.metal').write_text(source+'\n'+helper+'\n'+kernel+'\n')
 driver=driver_source.read_text().replace('kalandra_matmul_hardware_pair32','kalandra_matmul_hardware_reuse_'+a.layout)
 if a.layout=='quad':
  assert driver.count('((m+15)/16)')==1
  driver=driver.replace('((m+15)/16)','((m+(columns==32?31:15))/(columns==32?32:16))')
 (build/'Driver.mm').write_text(driver)
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',build/'study.metal','-o',build/'study.air'])
 p.run([*metal,build/'study.air','-o',library])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',build/'Driver.mm','-framework','Foundation','-framework','Metal','-o',binary])
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in (binary,library,build/'study.metal',build/'Driver.mm')},commands=p.COMMANDS),indent=2)+'\n')
saved=json.loads(manifest.read_text());assert saved['signature']==signature
for f,h in saved['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(16,512,512),(64,512,2048),(64,2048,512),(16,512,2048),(16,2048,512),(64,512,512)]
if a.phase=='prepare':
 checks=[]
 for shape in [(1,512,1),(3,512,17),(15,576,31),(17,1536,33),(17,4096,65),(33,2112,63),*shapes]:
  for pattern in range(3):
   for v in ('baseline','current'):
    p.observations(p.run([binary,library,v,*shape,1,1,pattern],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),shape[0]*shape[2],1,1)
    checks.append(dict(shape=shape,pattern=pattern,version=v,passed=True))
  print('Verified '+str(shape),flush=True)
 data=dict(contract=__doc__,layout=a.layout,checks=checks)
else:
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 q=build/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==72 and all(r['passed'] for r in qualified['checks'])
 def invoke(cell,count,rounds):return p.observations(p.run([binary,library,*cell,count,rounds,0]),cell[1]*cell[3],count,rounds)
 data=p.measure_paired([(v,*s) for s in shapes for v in ('baseline','current')],invoke,lambda c:c[1]*c[3],blocks=a.blocks,target_ms=30,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,layout=a.layout,qualification_sha256=p.sha(q))
for f,h in saved['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
p.evidence(a.output,data,sources+[manifest],[binary,library,build/'study.metal',build/'Driver.mm'],p.ROOT/'build/native/native/kalandra')
