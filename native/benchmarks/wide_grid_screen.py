"""Strict 2D-grid wide-output tile screen on large matrix workloads.

Both kernels use aligned grids matching production's current baseline. The
candidate shares A across two adjacent output cells, preserving every product
and adjacent-pair sum. This is an isolated screen, never whole-model acceptance.
"""
import argparse,json,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
from output_reuse_shader import append_pair32,function
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--blocks',type=int,default=5)
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists();b.mkdir(parents=True,exist_ok=True)
header=p.ROOT/'native/metal-source/HardwareShaders.h';driver=Path(__file__).with_name('OutputReuseStudy.mm')
sources=[Path(__file__),header,driver,Path(__file__).parent/'fair/output_reuse_shader.py',Path(p.__file__)]
sig={p.key(x.resolve()):p.sha(x) for x in sources};manifest=b/'manifest.json'
shapes=[(16,512,512),(64,512,512),(64,512,2048),(64,2048,512),(128,512,512),(128,512,2048),(128,2048,512),(512,512,512),(512,512,2048),(512,2048,512)]
oddchunks=[(128,576,64),(128,1536,32),(128,2112,64),(128,4096,32)]
if a.phase=='prepare':
 assert not manifest.exists()
 text=header.read_text();shader=lambda name:re.search(r'\b'+name+r' = R"MSL\((.*?)\)MSL";',text,re.S)[1]
 source=append_pair32('#include <metal_stdlib>\nusing namespace metal;\n'+shader('hardwareMatmulShader')+shader('hardwareRegisterMatmulShader'))
 for name,columns in [('register16',16),('pair32',32)]:
  body=function(source,'kernel void kalandra_matmul_hardware_'+name+'(')
  changed=body.replace('uint group [[threadgroup_position_in_grid]]','uint2 group [[threadgroup_position_in_grid]]').replace('thread_position_in_threadgroup','thread_index_in_threadgroup')
  changed=re.sub(r'  const uint tileColumns=[^;]+;\n','',changed)
  changed=changed.replace('rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*'+str(columns), 'rowBase=group.y*16, colBase=group.x*'+str(columns))
  changed=re.sub(r'(?:r<m|c<n) \? ([^;]+?) : 0.0f',r'\1',changed)
  changed=changed.replace('if(row<m && col<n) {','{').replace('if(col+1<n) output','output')
  assert changed!=body and 'tileColumns' not in changed
  source=source.replace(body,changed)
 (b/'study.metal').write_text(source)
 text=driver.read_text().replace('check(m && n && k>=512','check(m%16==0 && n%32==0 && m && n && k>=512')
 old='MTLSizeMake(((m+15)/16)*((n+columns-1)/columns),1,1)';assert text.count(old)==1
 (b/'Driver.mm').write_text(text.replace(old,'MTLSizeMake(n/columns,m/16,1)'))
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',b/'study.metal','-o',b/'study.air']);p.run([*metal,b/'study.air','-o',b/'study.metallib'])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',b/'Driver.mm','-framework','Foundation','-framework','Metal','-o',b/'screen'])
 files=[b/'study.metal',b/'Driver.mm',b/'screen',b/'study.metallib'];manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},commands=p.COMMANDS),indent=2)+'\n')
 checks=[]
 for shape in shapes+oddchunks:
  for pattern in range(3):
   for version in ('baseline','current'):
    p.observations(p.run([b/'screen',b/'study.metallib',version,*shape,1,1,pattern],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'),timeout=600),shape[0]*shape[2],1,1)
    checks.append(dict(shape=shape,pattern=pattern,version=version,passed=True))
  print(shape,'all finite/IEEE/signed-zero outputs and inputs verified',flush=True)
 data=dict(contract=__doc__,checks=checks)
else:
 saved=json.loads(manifest.read_text());assert saved['signature']==sig
 for name,digest in saved['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==84 and all(x['passed'] for x in qualified['checks'])
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 def invoke(cell,count,rounds):return p.observations(p.run([b/'screen',b/'study.metallib',*cell,count,rounds,0],timeout=600),cell[1]*cell[3],count,rounds)
 data=p.measure_paired([[v,*s] for s in shapes for v in ('baseline','current')],invoke,lambda c:c[1]*c[3],blocks=a.blocks,rounds=3,target_ms=100,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,qualification_sha256=p.sha(q))
saved=json.loads(manifest.read_text());assert saved['signature']==sig
for name,digest in saved['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],p.ROOT/'build/native/native/kalandra')
