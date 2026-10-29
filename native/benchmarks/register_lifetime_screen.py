"""Strict register-lifetime screen; keep the 64-leaf shared tile and both barriers.

Evaluate its two 32-leaf subtrees sequentially in a non-unrolled loop. Preserve
all multiplications, addition children and their left/right order. No production
change. Qualification checks full GPU bits, independent finite/signed-zero tree,
input immutability and guards. Timed batches repeat those checks outside timing.
"""
import argparse,json,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
from output_reuse_shader import function
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--phase',choices=('prepare','measure','capture'),required=True);parser.add_argument('--build',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--blocks',type=int,default=7);a=parser.parse_args();assert not a.output.exists()
b=a.build.resolve();b.mkdir(parents=True,exist_ok=True);header=p.ROOT/'native/metal-source/HardwareShaders.h';driver=Path(__file__).with_name('OutputReuseStudy.mm');manifest=b/'manifest.json'
sources=[Path(__file__),header,driver,Path(__file__).parent/'fair/output_reuse_shader.py',Path(__file__).parent/'fair/protocol.py',p.ROOT/'native/metal-proof/toolchain.json'];sig={p.key(f.resolve()):p.sha(f) for f in sources}
if not manifest.exists():
 assert a.phase=='prepare'
 text=header.read_text()
 def shader(name):return re.search(r'\b'+name+r' = R"MSL\((.*?)\)MSL";',text,re.S)[1]
 source='#include <metal_stdlib>\nusing namespace metal;\n'+shader('hardwareMatmulShader')+shader('hardwareRegisterMatmulShader')
 kernel=function(source,'kernel void kalandra_matmul_hardware_register16(')
 start=kernel.index('      float r0=');end=kernel.index('      // Explicit binary-counter roots',start)
 kernel=kernel[:start]+'''      float leftHalf=0.0f,value=0.0f;
      #pragma clang loop unroll(disable)
      for(uint phase=0;phase<2;++phase) {
        const uint offset=phase*32;
        float r0=kalandra_hardware_eight16(av,bv,lane/16,lane%16,offset);
        float r1=kalandra_hardware_eight16(av,bv,lane/16,lane%16,offset+8);
        float r2=kalandra_hardware_eight16(av,bv,lane/16,lane%16,offset+16);
        float r3=kalandra_hardware_eight16(av,bv,lane/16,lane%16,offset+24);
        float subtree=(r0+r1)+(r2+r3);
        if(phase==0) leftHalf=subtree;
        else value=leftHalf+subtree;
      }
'''+kernel[end:]
 kernel=kernel.replace('kalandra_matmul_hardware_register16(','kalandra_matmul_hardware_phased32(')
 (b/'study.metal').write_text(source+'\n'+kernel+'\n')
 d=driver.read_text().replace('kalandra_matmul_hardware_pair32','kalandra_matmul_hardware_phased32').replace('candidate?32:16','16')
 needle='    auto start=std::chrono::steady_clock::now();'
 assert d.count(needle)==1
 d=d.replace(needle,'''    auto manager=[MTLCaptureManager sharedCaptureManager];
    const char* destination=getenv("KALANDRA_PROBE_CAPTURE");bool capturing=round==0 && destination;
    if(capturing){auto desc=[MTLCaptureDescriptor new];desc.captureObject=queue;desc.destination=MTLCaptureDestinationGPUTraceDocument;desc.outputURL=[NSURL fileURLWithPath:@(destination)];check([manager startCaptureWithDescriptor:desc error:&error]);}
'''+needle)
 needle='    auto actual=download(out);';assert d.count(needle)==1;d=d.replace(needle,'    if(capturing) [manager stopCapture];\n'+needle)
 (b/'Driver.mm').write_text(d)
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.2','-fno-fast-math','-ffp-contract=off','-c',b/'study.metal','-o',b/'study.air']);p.run([*metal,b/'study.air','-o',b/'study.metallib'])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',b/'Driver.mm','-framework','Foundation','-framework','Metal','-o',b/'screen'])
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(f):p.sha(f) for f in (b/'study.metal',b/'Driver.mm',b/'screen',b/'study.metallib')},commands=p.COMMANDS),indent=2)+'\n')
frozen=json.loads(manifest.read_text());assert sig==frozen['signature']
for f,h in frozen['artifacts'].items():assert p.sha(p.ROOT/f)==h,f
shapes=[(16,512,512),(64,512,2048),(64,2048,512),(16,512,2048),(16,2048,512),(64,512,512)]
if a.phase=='prepare':
 checks=[]
 for shape in [(1,512,1),(3,512,17),(15,576,31),(17,1536,33),(17,4096,65),(33,2112,63),*shapes]:
  for pattern in range(3):
   for version in ('baseline','current'):
    p.observations(p.run([b/'screen',b/'study.metallib',version,*shape,1,1,pattern],env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),shape[0]*shape[2],1,1)
    checks.append(dict(shape=shape,pattern=pattern,version=version,passed=True))
  print('Verified '+str(shape),flush=True)
 data=dict(contract=__doc__,checks=checks)
else:
 q=b/'qualification.json';saved=json.loads(q.read_text());assert len(saved['checks'])==72 and all(r['passed'] for r in saved['checks'])
 if a.phase=='capture':
  trace=b/'phased32.gputrace';assert not trace.exists()
  log=p.run([b/'screen',b/'study.metallib','current',64,2048,512,1,1,0],env=dict(os.environ,MTL_CAPTURE_ENABLED='1',KALANDRA_PROBE_CAPTURE=str(trace)))
  p.observations(log,64*512,1,1);data=dict(acceptance=False,contract='Qualified single-dispatch resource capture; not timing acceptance.',log=log,trace=p.key(trace))
 else:
  for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES','KALANDRA_PROBE_CAPTURE'):assert os.getenv(flag) in (None,'','0')
  def invoke(cell,count,rounds):return p.observations(p.run([b/'screen',b/'study.metallib',*cell,count,rounds,0]),cell[1]*cell[3],count,rounds)
  data=p.measure_paired([(v,*s) for s in shapes for v in ('baseline','current')],invoke,lambda c:c[1]*c[3],blocks=a.blocks,target_ms=30,adaptive=True,max_iterations=256)
  data.update(contract=__doc__,acceptance=False,scope='Isolated kernel screen; full-model/runtime qualification still required.')
 data['qualification_sha256']=p.sha(q)
p.evidence(a.output,data,sources+[manifest],[p.ROOT/f for f in frozen['artifacts']],p.ROOT/'build/native/native/kalandra')
