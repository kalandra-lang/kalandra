"""Isolated 16x32 strict-output-reuse screen; no production dispatch changes.

Checks all result bits against the frozen register16 GPU entry and, for finite
and signed-zero patterns, an independent CPU adjacent-pair oracle. All inputs
and output guard bytes are checked outside each timed batch. This is a kernel
screen, not whole-program acceptance.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
from output_reuse_shader import append_pair32

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True)
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--blocks',type=int,default=5)
a=parser.parse_args();assert not a.output.exists()
build=a.build.resolve();build.mkdir(parents=True,exist_ok=True)
header=p.ROOT/'native/metal-source/HardwareShaders.h'
driver=Path(__file__).with_name('OutputReuseStudy.mm')
sources=[Path(__file__),driver,header,Path(__file__).parent/'fair/output_reuse_shader.py',Path(__file__).parent/'fair/protocol.py']
signature={p.key(f.resolve()):p.sha(f) for f in sources}
manifest=build/'manifest.json';binary=build/'screen';library=build/'study.metallib'
if not manifest.exists():
 assert a.phase=='prepare'
 text=header.read_text()
 def shader(name):return re.search(r'\b'+name+r' = R"MSL\((.*?)\)MSL";',text,re.S)[1]
 source='#include <metal_stdlib>\nusing namespace metal;\n'+shader('hardwareMatmulShader')+shader('hardwareRegisterMatmulShader')
 (build/'study.metal').write_text(append_pair32(source))
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text())
 metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',build/'study.metal','-o',build/'study.air'])
 p.run([*metal,build/'study.air','-o',library])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',binary])
 manifest.write_text(json.dumps(dict(signature=signature,artifacts={p.key(f):p.sha(f) for f in (binary,library,build/'study.metal')},commands=p.COMMANDS),indent=2)+'\n')
frozen=json.loads(manifest.read_text());assert frozen['signature']==signature
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(16,512,512),(64,512,2048),(64,2048,512),(16,512,2048),(16,2048,512),(64,512,512)]
if a.phase=='prepare':
 checks=[]
 for shape in [(1,512,1),(3,512,17),(15,576,31),(17,1536,33),(17,4096,65),(33,2112,63),*shapes]:
  for pattern in range(3):
   for version in ('baseline','current'):
    p.observations(p.run([binary,library,version,*shape,1,1,pattern],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),shape[0]*shape[2],1,1)
    checks.append(dict(shape=shape,pattern=pattern,version=version,passed=True))
  print('Verified '+str(shape),flush=True)
 data=dict(contract=__doc__,checks=checks,manifest=p.key(manifest))
else:
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 qualified=json.loads((build/'qualification.json').read_text());assert len(qualified['checks'])==72 and all(r['passed'] for r in qualified['checks'])
 def invoke(cell,count,rounds):
  return p.observations(p.run([binary,library,*cell,count,rounds,0]),cell[1]*cell[3],count,rounds)
 cells=[(v,*s) for s in shapes for v in ('baseline','current')]
 data=p.measure_paired(cells,invoke,lambda c:c[1]*c[3],blocks=a.blocks,target_ms=30,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,qualification_sha256=p.sha(build/'qualification.json'))
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
p.evidence(a.output,data,sources+[manifest],[binary,library,build/'study.metal'],p.ROOT/'build/native/native/kalandra')
