"""Qualify production Steel integration on a complete resident transformer block.

Numerical policy is explicit: optimized fused float32 versus fixed-tree. Frozen
legacy production and MPSGraph are references, not production dependencies.
Run acceptance sequentially after all builds/tests/validation finish.
"""
import argparse,json,os,re,shutil,struct,sys
from pathlib import Path
os.environ['OPENBLAS_NUM_THREADS']='1'
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
import prepared_program
import transformer_reference as ref
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('aa','frozen','policy','repeat','mps'),default='frozen')
ap.add_argument('--baseline',type=Path,default=p.ROOT/'benchmarks/.build/gpu-prepared-output-fastmiss-frozen-2026-10-29/gpu')
ap.add_argument('--baseline-bundle',type=Path,default=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29/bundle')
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
for flag in ('KALANDRA_FIXED_TREE','KALANDRA_NO_OUTPUT_CACHE','KALANDRA_UPLOAD_CACHE_BYTES',
             'KALANDRA_DEVICE_CACHE_BYTES','KALANDRA_PAIR_REVERSE'):
 assert os.getenv(flag) is None, 'Benchmark controls must be selected by this script: '+flag
fair=Path(__file__).parent/'fair';compiler=p.ROOT/'build/native/native/kalandra'
baseline=a.baseline.resolve();oldbundle=a.baseline_bundle.resolve()
configuration=dict(baseline=str(baseline),baseline_bundle=str(oldbundle))
driver_source=fair/'TransformerSteel.cpp'
block=p.ROOT/'native/benchmarks/TransformerBlock.k'
sources=[Path(__file__),Path(p.__file__),Path(prepared_program.__file__),Path(ref.__file__),fair/'ffn_reference.py',fair/'TransformerBlock.h',fair/'ResidualFfn.h',fair/'TransformerMps.mm',block,driver_source,
 p.ROOT/'native/metal/Metal.mm',p.ROOT/'native/metal/Metal.h',p.ROOT/'native/metal/MatmulLayout.h',
 p.ROOT/'native/metal-source/SteelMatmul.h',p.ROOT/'native/metal-source/MatmulEpilogue.h',p.ROOT/'native/metal-source/Build.cpp',p.ROOT/'native/metal-source/Compiler.cpp',
 p.ROOT/'native/include/kalandra/Arithmetic.h',p.ROOT/'native/third_party/mlx-steel/SteelUpstream.h',p.ROOT/'native/third_party/mlx-steel/UPSTREAM.json',p.ROOT/'native/third_party/mlx-steel/LICENSE']
sources += [p.ROOT/'native/metal/ElementwiseLayout.h',
            p.ROOT/'native/toolchain-proof/toolchain.json',p.ROOT/'native/metal-proof/toolchain.json',
            p.ROOT/'build/native/CMakeCache.txt',
            *sorted((p.ROOT/'native/runtime').glob('*.h')),
            *sorted((p.ROOT/'native/include/kalandra').glob('*.h'))]
sources=list(dict.fromkeys(sources))
sig={p.key(x.resolve()):p.sha(x) for x in sources};manifest=b/'manifest.json'
variants={'optimized':(['-O2'],'libkalandra-task-core-optimized.a'),
 'debug':(['-O0'],'libkalandra-native-runtime.a'),
 'sanitized':(['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer'],'libkalandra-native-runtime-sanitized.a'),
 'thread-sanitized':(['-O1','-g','-fsanitize=thread','-fno-omit-frame-pointer'],'libkalandra-native-runtime-thread-sanitized.a')}
def load(path):return json.loads(path.read_text())
if a.phase=='prepare':
 assert baseline.is_file() and any(oldbundle.glob('*.metallib')), 'Provide the frozen baseline executable and shader bundle'
 b.mkdir(parents=True,exist_ok=False)
 cache=(p.ROOT/'build/native/CMakeCache.txt').read_text()
 configured=lambda name:re.search(r'^'+name+r':[^=]+=(.+)$',cache,re.M)[1]
 metal=Path(configured('KALANDRA_METAL_COMPILER'))
 platform_tools=dict(metal_compiler=str(metal),metal_sha256=p.sha(metal),
   metal_version=p.run([metal,'--version']).strip(),sdk=configured('KALANDRA_METAL_SDK'),
   xcode=p.run(['xcodebuild','-version']).strip(),steel_language='metal3.2',fast_math=False,fp_contract='off')
 assert load(p.ROOT/'native/metal-proof/toolchain.json')['metalCompilerVersion'] in platform_tools['metal_version']
 gpu_source=b/'block-gpu.k';gpu_source.write_text(prepared_program.gpu_source(block.read_text()))
 p.run([compiler,gpu_source,'--target','metal','--arithmetic','hardware','--matmul-policy','optimized','-O2','-o',b/'bundle'],timeout=900)
 host=(b/'bundle/host.cpp').read_text()
 assert host.count('m::NumericInstructionKind::Map,')==8 and host.count('m::NumericInstructionKind::Matmul,')==8
 (b/'TransformerProgram.h').write_text(prepared_program.header(host))
 assert (b/'bundle/THIRD_PARTY_NOTICES.txt').read_bytes()==(p.ROOT/'native/third_party/mlx-steel/LICENSE').read_bytes()
 assert 'matmul_policy=optimized' in (b/'bundle/manifest.txt').read_text()
 for name in ('Driver.cpp','Verify.cpp'):
  shutil.copy2(driver_source,b/name)
 shutil.copy2(compiler,b/'kalandra')
 shutil.copy2(p.ROOT/'native/metal/Metal.h',b/'Metal.h')
 artifacts=[b/'kalandra',b/'Metal.h',b/'Driver.cpp',b/'Verify.cpp',gpu_source,b/'TransformerProgram.h',b/'bundle/host.cpp',b/'bundle/manifest.txt',b/'bundle/THIRD_PARTY_NOTICES.txt',baseline,*(oldbundle.glob('*.metallib'))]
 for variant,(flags,taskname) in variants.items():
  metal=p.ROOT/f'build/native/native/metal/libkalandra-metal-{variant}.a';task=p.ROOT/'build/native/native'/taskname
  shutil.copy2(metal,b/f'metal-{variant}.a');shutil.copy2(task,b/f'task-{variant}.a')
  common=[p.CLANG,'-std=c++23','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0',*flags,'-I'+str(b),'-I'+str(p.ROOT/'native/metal'),'-I'+str(fair)]
  link=[b/f'metal-{variant}.a',b/f'task-{variant}.a','-framework','Foundation','-framework','Metal']
  p.run([*common,'-DKALANDRA_STEEL_VERIFY',b/'Verify.cpp',*link,'-o',b/f'verify-{variant}'])
  artifacts += [b/f'metal-{variant}.a',b/f'task-{variant}.a',b/f'verify-{variant}']
  if variant=='optimized':p.run([*common,b/'Driver.cpp',*link,'-o',b/'gpu']);artifacts.append(b/'gpu')
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks','-ffp-contract=off','-mmacosx-version-min=26.0',fair/'TransformerMps.mm','-framework','Foundation','-framework','Metal','-framework','MetalPerformanceShaders','-framework','MetalPerformanceShadersGraph','-o',b/'mps'])
 artifacts += [b/'mps',*(b/'bundle').glob('*.metal'),*(b/'bundle').glob('*.metallib')]
 manifest.write_text(json.dumps(dict(signature=sig,configuration=configuration,platform_tools=platform_tools,artifacts={p.key(x):p.sha(x) for x in artifacts},commands=p.COMMANDS),indent=2)+'\n')
 a.output.write_text(manifest.read_text());sys.exit(0)
saved=load(manifest);assert saved['signature']==sig and saved['configuration']==configuration
for path,digest in saved['artifacts'].items():assert p.sha(p.ROOT/path)==digest,path
decode=lambda text:json.loads(text,parse_int=lambda n:-0.0 if n=='-0' else int(n))
bits=lambda row:struct.pack('<'+'f'*len(row['cells']),*row['cells'])
def command(kind,m,d,count=1,rounds=1,mode=0,dump=0,pattern=0,policy=2):
 if kind=='mps':return [b/'mps',m,d,count,rounds,mode,dump,pattern,1]
 return [baseline if kind=='frozen' else b/'gpu',oldbundle if kind=='frozen' else b/'bundle',m,d,count,rounds,mode,dump,pattern,policy,1]
if a.phase=='verify':
 checks=[];oracles=[]
 shapes=[(1,512),(16,512),(64,512),(64,256),(3,33),(7,65),(1,17),(31,31),(512,512)]
 for m,d in shapes:
  expected={}
  for fixed in (False,True):
   for pattern in (0,1):
    env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    if fixed:env['KALANDRA_FIXED_TREE']='1'
    row=decode(p.run(command('current',m,d,dump=1,pattern=pattern),env=env,timeout=300));metrics,_=ref.assess(m,d,pattern,row);expected[fixed,pattern]=bits(row)
    if fixed:
     reference=decode(p.run(command('frozen',m,d,dump=1,pattern=pattern),env=env,timeout=300));assert bits(reference)==bits(row)
    oracles.append(dict(m=m,d=d,fixed=fixed,pattern=pattern,metrics=metrics))
  # Validate all execution policies, uploads, opposite input poisoning and
  # private/shared cache quotas against freshly qualified own-policy outputs.
  for cap in (0,4096,16*1024*1024):
   for reverse in (False,True):
    for policy in (0,1,2):
     for mode in (0,1,2):
      env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1',KALANDRA_DEVICE_CACHE_BYTES=str(cap))
      if reverse:env['KALANDRA_PAIR_REVERSE']='1'
      cmd=[b/'verify-optimized',b/'bundle',m,d,1,1,mode,1,0,policy,1]
      rows=[decode(s) for s in p.run(cmd,env=env,timeout=300).splitlines()];assert len(rows)==4
      assert all(bits(row)==expected[False,i%2] for i,row in enumerate(rows))
      checks.append(dict(m=m,d=d,cap=cap,reverse=reverse,policy=policy,mode=mode,comparisons=4,passed=True))
  print(m,d,'both numerical policies and all changing-input execution/cache modes pass',flush=True)
 # All four native modes additionally exercise eligible ragged and dense graphs.
 mode_checks=[]
 for variant in variants:
  for m,d in ((3,33),(64,65),(64,512),(512,512)):
   for fixed in (False,True):
    env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    if fixed:env['KALANDRA_FIXED_TREE']='1'
    rows=[decode(s) for s in p.run([b/f'verify-{variant}',b/'bundle',m,d,1,1,0,1,0,2,1],env=env,timeout=300).splitlines()];assert len(rows)==4
    metrics=[]
    for i,row in enumerate(rows):
     metric,_=ref.assess(m,d,i%2,row);metrics.append(metric)
    mode_checks.append(dict(variant=variant,m=m,d=d,fixed=fixed,metrics=metrics,comparisons=4))
  print(variant,'prepared full-output checks pass',flush=True)
 references=[]
 for m,d in ((1,512),(64,512),(512,512)):
  for mode in (0,2):
   for pattern in (0,1):
    row=decode(p.run(command('mps',m,d,mode=mode,dump=1,pattern=pattern),timeout=300,
               env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
    metric,_=ref.assess(m,d,pattern,row)
    references.append(dict(m=m,d=d,mode=mode,pattern=pattern,metrics=metric))
 data=dict(contract=__doc__,production_change=True,checks=checks,oracles=oracles,mode_checks=mode_checks,references=references)
else:
 qpath=b/'qualification.json';q=load(qpath);assert len(q['checks'])==486 and all(x['passed'] for x in q['checks'])
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 caches=[]
 def invoke(cell,count,rounds):
  version,m,d,mode=cell;kind='current'
  env=dict(os.environ)
  if a.study=='frozen' and version=='baseline' or a.study=='repeat' and version=='baseline':kind='frozen'
  elif a.study=='policy' and version=='baseline':env['KALANDRA_FIXED_TREE']='1'
  elif a.study=='mps' and version=='current':kind='mps'
  rows=[json.loads(s) for s in p.run(command(kind,m,d,count,rounds,mode),env=env,timeout=900).splitlines()]
  cache=[x for x in rows if x.get('cache')];timed=[x for x in rows if 'ns' in x]
  for x in cache:assert x['retained_bytes']==x['live_bytes']+x['cached_bytes'] and x['cached_bytes']<=16*1024*1024 and x['peak_retained_bytes']<=64*1024*1024
  caches.append(dict(cell=cell,kind=kind,iterations=count,samples=cache))
  return p.observations('\n'.join(map(json.dumps,timed)),m*d*4,count,rounds)
 groups=[(1,512,0),(64,512,0),(512,512,0),(512,512,2)]
 data=p.measure_paired([[v,*g] for g in groups for v in ('baseline','current')],invoke,lambda c:c[1]*c[2]*4,blocks=3,rounds=3,target_ms=500,adaptive=True,max_iterations=8192)
 data.update(contract=__doc__,production_change=True,study=a.study,caches=caches,qualification_sha256=p.sha(qpath))
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],b/'kalandra')
