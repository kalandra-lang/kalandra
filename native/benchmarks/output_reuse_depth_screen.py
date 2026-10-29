"""Strict FFN controls: row reuse versus smaller/larger shared K chunks.

No production changes. Each candidate retains the exact adjacent-pair tree.
The 128-leaf candidate falls back to register16 when K is not divisible by 128.
"""
import argparse,json,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
from output_reuse_shader import function
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase',choices=('prepare','measure'),required=True)
parser.add_argument('--layout',choices=('rows','leaf32','leaf128'),required=True)
parser.add_argument('--output',type=Path,required=True);parser.add_argument('--blocks',type=int,default=5)
a=parser.parse_args();assert not a.output.exists()
base=p.ROOT/'benchmarks/.build/gpu-output-reuse-2026-10-29';build=base/a.layout;build.mkdir(parents=True,exist_ok=True)
frozen=base/'screen/study.metal';driver_source=Path(__file__).with_name('OutputReuseStudy.mm')
sources=[Path(__file__),driver_source,frozen,Path(__file__).parent/'fair/output_reuse_shader.py',Path(__file__).parent/'fair/protocol.py']
signature={p.key(f.resolve()):p.sha(f) for f in sources};manifest=build/'manifest.json'
binary=build/'screen';library=build/'study.metallib'
if not manifest.exists():
 assert a.phase=='prepare';source=frozen.read_text()
 helper=function(source,'inline float kalandra_hardware_eight16(')
 kernel=function(source,'kernel void kalandra_matmul_hardware_register16(')
 helper=helper.replace('eight16(', 'eight_'+a.layout+'(')
 kernel=kernel.replace('hardware_register16(', 'hardware_'+a.layout+'(').replace('eight16(', 'eight_'+a.layout+'(')
 driver=driver_source.read_text().replace('kalandra_matmul_hardware_pair32','kalandra_matmul_hardware_'+a.layout)
 if a.layout=='rows':
  helper=helper.replace('inline float ','inline float2 ',1)
  helper=re.sub(r'float p([0-7])=',r'float2 p\1=',helper)
  helper,n=re.subn(r'a\[row\*64\+first\+([0-7])\]',lambda m:f'float2(a[row*64+first+{m[1]}],a[(row+16)*64+first+{m[1]}])',helper);assert n==8
  kernel=kernel.replace('av[1024]','av[2048]').replace('rowBase=(group/tileColumns)*16','rowBase=(group/tileColumns)*32')
  old='''      const uint r=rowBase+at/64,c=colBase+at%16;
      av[at]=r<m ? a.values[r*k+first+at%64] : 0.0f;
      bv[at]=c<n ? b.values[(first+at/16)*n+c] : 0.0f;'''
  new='''      const uint c=colBase+at%16;
      bv[at]=c<n ? b.values[(first+at/16)*n+c] : 0.0f;
    }
    for(uint at=lane;at<2048;at+=256) {
      const uint r=rowBase+at/64;
      av[at]=r<m ? a.values[r*k+first+at%64] : 0.0f;'''
  assert old in kernel;kernel=kernel.replace(old,new)
  kernel=kernel.replace('float s0=','float2 s0=').replace('float value=','float2 value=')
  kernel=re.sub(r'float r([0-7])=',r'float2 r\1=',kernel)
  kernel=kernel.replace('output.values[row*n+col]=value;', 'output.values[row*n+col]=value.x;\n    if(row+16<m) output.values[(row+16)*n+col]=value.y;')
  driver=driver.replace('((m+15)/16)','((m+(columns==32?31:15))/(columns==32?32:16))').replace('((n+columns-1)/columns)','((n+15)/16)')
 else:
  leaf=32 if a.layout=='leaf32' else 128
  helper=helper.replace('row*64','row*'+str(leaf))
  kernel=kernel.replace('av[1024],bv[1024]',f'av[{16*leaf}],bv[{16*leaf}]').replace('blocks=k/64',f'blocks=k/{leaf}').replace('first=block*64',f'first=block*{leaf}').replace('at<1024',f'at<{16*leaf}').replace('at/64',f'at/{leaf}').replace('at%64',f'at%{leaf}')
  expression='((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7))'
  if leaf==32:
   kernel=re.sub(r'      float r[4-7]=[^;]+;\n','',kernel)
   assert expression in kernel;kernel=kernel.replace(expression,'(r0+r1)+(r2+r3)')
   kernel=kernel.replace('s6=0.0f;','s6=0.0f,s7=0.0f;').replace('else s6=s5+value;','else { value=s5+value; if(!(block&64u)) s6=value; else s7=s6+value; }')
   kernel=kernel.replace('output.values[row*n+col]=value;','if(blocks&128u) value=used?s7+value:s7;\n    output.values[row*n+col]=value;')
  else:
   extra=''.join(f'      float r{i}=kalandra_hardware_eight_leaf128(av,bv,lane/16,lane%16,{i*8});\n' for i in range(8,16))
   kernel=kernel.replace('      float value=',extra+'      float value=',1)
   kernel=kernel.replace(expression,'('+expression+')+(((r8+r9)+(r10+r11))+((r12+r13)+(r14+r15)))')
   driver=driver.replace('const bool candidate =','bool candidate =').replace('check(m && n &&', 'candidate = candidate && k%128==0;\n  check(m && n &&')
  driver=driver.replace('candidate?32:16','16')
 (build/'study.metal').write_text(source+'\n'+helper+'\n'+kernel+'\n');(build/'Driver.mm').write_text(driver)
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',build/'study.metal','-o',build/'study.air']);p.run([*metal,build/'study.air','-o',library])
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
