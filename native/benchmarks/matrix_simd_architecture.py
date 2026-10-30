"""Benchmark-only SIMD-group matrix architecture experiment.

This deliberately permits a different floating reduction/accumulation order.
It is NOT an implementation of the existing strict matmul contract and must not
be selected by production. Every cell meets a finite-input float64 error bound;
bit differences (including signed zero) are reported, never silently accepted
as strict equivalence. This isolated kernel screen is not language throughput.
"""
import argparse,json,os,re,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'));import protocol as p
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','measure'),required=True);ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True);assert not a.output.exists()
base=p.ROOT/'benchmarks/.build/gpu-wide-grid-2026-10-30';driver=Path(__file__).with_name('OutputReuseStudy.mm')
sources=[Path(__file__),driver,base/'study.metal',Path(p.__file__)]
sig={p.key(x.resolve()):p.sha(x) for x in sources};manifest=b/'manifest.json'
shapes=[(64,512,2048),(64,2048,512),(512,512,512),(512,512,2048),(512,2048,512)]
if a.phase=='prepare':
 assert not manifest.exists()
 source=(base/'study.metal').read_text()+r'''
#include <metal_simdgroup_matrix>
kernel void kalandra_simdgroup_matrix(
 device KalandraHardwareMatrix& a [[buffer(0)]],
 device KalandraHardwareMatrix& b [[buffer(1)]],
 device KalandraHardwareMatrix& output [[buffer(2)]],
 device KalandraHardwareShape& parameters [[buffer(3)]],
 uint2 group [[threadgroup_position_in_grid]]) {
 const uint k=parameters.values[1], n=parameters.values[2];
 const uint row=group.y*8, col=group.x*8;
 simdgroup_float8x8 acc=make_filled_simdgroup_matrix<float,8,8>(0.0f);
 for(uint first=0;first<k;first+=8) {
  simdgroup_float8x8 av,bv;
  simdgroup_load(av,a.values+row*k+first,k);
  simdgroup_load(bv,b.values+first*n+col,n);
  simdgroup_multiply_accumulate(acc,av,bv,acc);
 }
 simdgroup_store(acc,output.values+row*n+col,n);
}
'''
 (b/'study.metal').write_text(source)
 text=driver.read_text().replace('check(p.maxTotalThreadsPerThreadgroup>=256);','check(p.maxTotalThreadsPerThreadgroup>=32);').replace('kalandra_matmul_hardware_pair32','kalandra_simdgroup_matrix')
 text=text.replace('check(m && n && k>=512','check(m%16==0 && n%16==0 && pattern!=1 && m && n && k>=512')
 old='MTLSizeMake(((m+15)/16)*((n+columns-1)/columns),1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)';assert text.count(old)==1
 text=text.replace(old,'MTLSizeMake(n/columns,m/columns,1) threadsPerThreadgroup:MTLSizeMake(columns==16?256:32,1,1)')
 text=text.replace('candidate?32:16','candidate?8:16')
 text=text.replace('auto e=[cb computeCommandEncoder];check(e);', 'check(pipeline.maxTotalThreadsPerThreadgroup>=(columns==16?256:32));auto e=[cb computeCommandEncoder];check(e);')
 text=text.replace('#include <algorithm>','#include <algorithm>\n#include <source_location>')
 text=text.replace('static void check(bool value)', 'static void check(bool value,std::source_location location=std::source_location::current())').replace('if (!value) throw', 'if (!value) {fprintf(stderr,\"check failed at line %u\\n\",location.line());throw').replace('std::runtime_error(\"output reuse screen failed\"); }','std::runtime_error(\"output reuse screen failed\");} }')
 needle='  for(int round=-2;';assert text.count(needle)==1
 oracle=r'''  std::vector<double> exact(oc),bound(oc);
  const double u=std::ldexp(1.0,-24), gamma=(k*u)/(1-k*u);
  for(unsigned r=0;r<m;++r)for(unsigned c=0;c<n;++c) {
    double value=0,absolute=0;
    for(unsigned i=0;i<k;++i) {double product=double(av[size_t(r)*k+i])*double(bv[size_t(i)*n+c]);value+=product;absolute+=std::abs(product);}
    exact[size_t(r)*n+c]=value;bound[size_t(r)*n+c]=gamma*absolute+std::ldexp(1.0,-100);
  }
'''
 text=text.replace(needle,oracle+needle)
 old='auto actual=download(out);check(memcmp(actual,expected.data(),oc*4)==0);';assert text.count(old)==1
 new='''auto actual=download(out);size_t different=0;double maxRatio=0,maxError=0;
    for(size_t cell=0;cell<oc;++cell) {different+=actual[cell]!=expected[cell];
      double value=std::bit_cast<float>(actual[cell]);check(std::isfinite(value));
      double error=std::abs(value-exact[cell]);maxError=std::max(maxError,error);maxRatio=std::max(maxRatio,error/bound[cell]);check(error<=bound[cell]);}
    if(!candidate)check(!different);
    fprintf(stderr,"{\\\"different_bits\\\":%zu,\\\"cells\\\":%zu,\\\"max_abs_error\\\":%.17g,\\\"max_error_bound_ratio\\\":%.17g}\\n",different,oc,maxError,maxRatio);'''
 text=text.replace(old,new);(b/'Driver.mm').write_text(text)
 lock=json.loads((p.ROOT/'native/metal-proof/toolchain.json').read_text());metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-mmacosx-version-min=26.0','-std=macos-metal2.3','-fno-fast-math','-ffp-contract=off','-c',b/'study.metal','-o',b/'study.air']);p.run([*metal,b/'study.air','-o',b/'study.metallib'])
 p.run([p.CLANG,'-std=c++23','-O2','-ffp-contract=off','-fobjc-arc','-fblocks',b/'Driver.mm','-framework','Foundation','-framework','Metal','-o',b/'screen'])
 files=[b/'study.metal',b/'Driver.mm',b/'screen',b/'study.metallib'];manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},commands=p.COMMANDS),indent=2)+'\n')
 checks=[]
 for shape in shapes:
  for pattern in (0,2):
   for version in ('baseline','current'):
    cmd=list(map(str,[b/'screen',b/'study.metallib',version,*shape,1,1,pattern]));p.COMMANDS.append(cmd)
    import subprocess
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=600,env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'));assert r.returncode==0,(cmd,r.stderr[-3000:])
    p.observations(r.stdout,shape[0]*shape[2],1,1)
    rows=[json.loads(x) for x in r.stderr.splitlines() if x.startswith('{"different_bits":')];assert len(rows)==3 and all(x['max_error_bound_ratio']<=1 for x in rows)
    checks.append(dict(shape=shape,pattern=pattern,version=version,checks=rows))
  print(shape,'finite accuracy, strict baseline and guards checked',flush=True)
 data=dict(contract=__doc__,checks=checks,production_change=False)
else:
 frozen=json.loads(manifest.read_text());assert frozen['signature']==sig
 for name,digest in frozen['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
 q=b/'qualification.json';qualified=json.loads(q.read_text());assert len(qualified['checks'])==20
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 def invoke(cell,count,rounds):return p.observations(p.run([b/'screen',b/'study.metallib',*cell,count,rounds,0],timeout=600),cell[1]*cell[3],count,rounds)
 data=p.measure_paired([[v,*s] for s in shapes for v in ('baseline','current')],invoke,lambda c:c[1]*c[3],blocks=5,rounds=3,target_ms=100,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,qualification_sha256=p.sha(q),production_change=False)
frozen=json.loads(manifest.read_text());assert frozen['signature']==sig
for name,digest in frozen['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in frozen['artifacts']],p.ROOT/'build/native/native/kalandra')
