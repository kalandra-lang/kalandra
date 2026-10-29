"""Frozen strict three-projection screen: separate encoders vs one batched dispatch.

The serial control uses three unchanged dispatches in one encoder. All three
outputs, immutable inputs and guards are checked outside timings. This does not
qualify graph admission, cancellation, ownership, or whole-transformer speed.
"""
import argparse,json,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
from output_reuse_shader import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True)
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--control',choices=('split','serial'),default='split')
parser.add_argument('--blocks',type=int,default=7)
a=parser.parse_args();assert not a.output.exists()
build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
header=p.ROOT/'native/metal-source/HardwareShaders.h';driver=Path(__file__).with_name('QkvDispatchStudy.mm')
sources=[Path(__file__),driver,header,Path(__file__).parent/'fair/output_reuse_shader.py',Path(__file__).parent/'fair/protocol.py',p.ROOT/'native/metal-proof/toolchain.json']
signature={p.key(f.resolve()):p.sha(f) for f in sources}
manifest=build/'manifest.json';binary=build/'screen';library=build/'study.metallib'
if not manifest.exists():
 assert a.phase=='prepare'
 text=header.read_text()
 def shader(name):return re.search(r'\b'+name+r' = R"MSL\((.*?)\)MSL";',text,re.S)[1]
 source='#include <metal_stdlib>\nusing namespace metal;\n'+shader('hardwareMatmulShader')+shader('hardwareRegisterMatmulShader')
 kernel=function(source,'kernel void kalandra_matmul_hardware_register16(')
 kernel=kernel.replace('kalandra_matmul_hardware_register16','kalandra_matmul_hardware_qkv')
 kernel=kernel.replace('Matrix& b [[buffer(1)]]','Matrix& b0 [[buffer(1)]]').replace('Matrix& output [[buffer(2)]]','Matrix& output0 [[buffer(2)]]')
 kernel=kernel.replace('uint lane [[thread_position_in_threadgroup]],','''device KalandraHardwareMatrix& b1 [[buffer(4)]],
    device KalandraHardwareMatrix& output1 [[buffer(5)]],
    device KalandraHardwareMatrix& b2 [[buffer(6)]],
    device KalandraHardwareMatrix& output2 [[buffer(7)]],
    uint lane [[thread_position_in_threadgroup]],''')
 kernel=kernel.replace('uint group [[threadgroup_position_in_grid]]) {','''uint2 groupIndex [[threadgroup_position_in_grid]]) {
  const uint group=groupIndex.x;
  device float* weights=groupIndex.y==0?b0.values:(groupIndex.y==1?b1.values:b2.values);
  device float* target=groupIndex.y==0?output0.values:(groupIndex.y==1?output1.values:output2.values);''')
 kernel=kernel.replace('uint lane [[thread_position_in_threadgroup]]','uint2 laneIndex [[thread_position_in_threadgroup]]').replace('const uint group=groupIndex.x;', 'const uint group=groupIndex.x, lane=laneIndex.x;')
 kernel=kernel.replace('b.values[','weights[').replace('output.values[','target[')
 (build/'study.metal').write_text(source+'\n'+kernel+'\n')
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',build/'study.metal','-o',build/'study.air'])
 p.run([*metal,build/'study.air','-o',library])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in (binary,library,build/'study.metal')},commands=p.COMMANDS),indent=2)+'\n')
frozen=json.loads(manifest.read_text());assert frozen['signature']==signature
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(16,512,512),(64,512,512),(16,1024,1024),(64,1024,1024)]
if a.phase=='prepare':
 checks=[]
 for shape in [(1,512,1),(3,576,17),(17,1536,33),(17,4096,65),(33,2112,63),*shapes]:
  for pattern in range(3):
   for version in ('baseline','serial','current'):
    p.observations(p.run([binary,library,version,*shape,1,1,pattern],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),3*shape[0]*shape[2],1,1)
    checks.append(dict(shape=shape,pattern=pattern,version=version,passed=True))
  print('Verified '+str(shape),flush=True)
 data=dict(contract=__doc__,checks=checks,manifest=p.key(manifest))
else:
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0'),flag
 q=build/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==81 and all(r['passed'] for r in qualified['checks'])
 def invoke(cell,count,rounds):
  version='serial' if cell[0]=='baseline' and a.control=='serial' else cell[0]
  return p.observations(p.run([binary,library,version,*cell[1:],count,rounds,0]),3*cell[1]*cell[3],count,rounds)
 data=p.measure_paired([(v,*s) for s in shapes for v in ('baseline','current')],invoke,lambda c:3*c[1]*c[3],blocks=a.blocks,target_ms=30,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,control=a.control,qualification_sha256=p.sha(q))
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
p.evidence(a.output,data,sources+[manifest],[binary,library,build/'study.metal'],p.ROOT/'build/native/native/kalandra')
