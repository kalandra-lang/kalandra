"""Pinned upstream MLX Steel vs current and experimental own-code kernels.

Isolated preallocated resident float32 kernels, not MLX-framework or whole-model
throughput. Upstream GEMMKernel is unmodified; only its Metal entry/ABI adapter
is ours. No production integration or numerical contract change.
"""
import argparse,json,math,os,random,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
PIN='1ba7f969ad05d86decfeeb83787cc9b79e440962'
U=p.ROOT/'benchmarks/.build/mlx-steel-source-2026-10-30'
F=p.ROOT/'benchmarks/.build/gpu-matrix-simd-block32-2026-10-30'
TILES={'steel64':(64,64,16,2,2),'steel32x64':(32,64,16,1,2),'steel64x32':(64,32,32,2,2)}
SHAPES=[(64,512,2048),(64,2048,512),(512,512,512),(512,512,2048),(512,2048,512)]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','verify','measure'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--study',choices=('aa','screen','confirm','prototype','repeat'),default='screen')
ap.add_argument('--selection',type=Path)
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists();b.mkdir(parents=True,exist_ok=True)
driver=Path(__file__).with_name('SteelMatrixStudy.mm');manifest=b/'manifest.json'
sources=[Path(__file__),driver,Path(p.__file__),U/'LICENSE',F/'study.metal',*(U/'mlx/backend/metal/kernels').rglob('*.h')]
signature={p.key(x.resolve()):p.sha(x) for x in sources}
def read(path):return json.loads(path.read_text())
def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def words(path):
 raw=path.read_bytes();return list(struct.unpack('<'+'I'*(len(raw)//4),raw))
def category(v):
 if v&0x7f800000==0x7f800000:return 'nan' if v&0x7fffff else '-inf' if v>>31 else '+inf'
 return ('-zero' if v>>31 else '+zero') if v&0x7fffffff==0 else 'subnormal' if v&0x7f800000==0 else 'normal'
def integer(v):
 e=(v>>23)&255;assert e!=255
 x=(v&0x7fffff) if not e else ((v&0x7fffff)|0x800000)<<(e-1)
 return -x if v>>31 else x
def oracle(inputs,shape):
 m,k,n=shape;av=list(map(integer,inputs[:m*k]));bv=list(map(integer,inputs[m*k:]));cache={};exact=[];absolute=[]
 cols=[tuple(bv[i*n+c] for i in range(k)) for c in range(n)]
 for r in range(m):
  row=tuple(av[r*k:(r+1)*k])
  for col in cols:
   key=(row,col)
   if key not in cache:
    products=[x*y for x,y in zip(row,col)];cache[key]=(sum(products),sum(map(abs,products)))
   x,total=cache[key];exact.append(x);absolute.append(total)
 return exact,absolute
def assess(actual,exact,absolute,k):
 indices=[i for i,w in enumerate(actual) if category(w) not in ('nan','+inf','-inf')]
 errors=[abs((integer(actual[i])<<149)-exact[i]) for i in indices]
 return dict(nonfinite_cells=len(actual)-len(indices),
  bound_failures=sum(e*((1<<24)-k)>k*absolute[i] for i,e in zip(indices,errors)),
  max_relative_error=max((e/abs(exact[i]) for i,e in zip(indices,errors) if exact[i]),default=None),
  max_absolute_error=math.ldexp(float(max(errors)),-298) if errors else None)
def command(variant,shape,inputfile,count,rounds,dump=0,output='/dev/null'):
 return [b/'study',b/'steel.metallib' if variant.startswith('steel') else b/'own.metallib',variant,*shape,inputfile,output,count,rounds,dump]

if a.phase=='prepare':
 assert not manifest.exists()
 assert p.run(['git','-C',U,'rev-parse','HEAD']).strip()==PIN
 assert not p.run(['git','-C',U,'diff','--stat']).strip()
 shader='#include "mlx/backend/metal/kernels/utils.h"\n#include "mlx/backend/metal/kernels/steel/gemm/gemm.h"\n'
 for name,(bm,bn,bk,wm,wn) in TILES.items():
  for mn in (0,1):
   for kk in (0,1):
    shader+=f'''\nkernel void {name}_{mn}_{kk}(const device float* A [[buffer(0)]],const device float* B [[buffer(1)]],device float* D [[buffer(2)]],constant mlx::steel::GEMMParams* params [[buffer(3)]],uint lane [[thread_index_in_simdgroup]],uint sg [[simdgroup_index_in_threadgroup]],uint3 group [[threadgroup_position_in_grid]],uint3 lid [[thread_position_in_threadgroup]]) {{
 using K=mlx::steel::GEMMKernel<float,float,{bm},{bn},{bk},{wm},{wn},false,false,{str(bool(mn)).lower()},{str(bool(kk)).lower()},float>;
 threadgroup float aa[K::tgp_mem_size_a],bb[K::tgp_mem_size_b];
 K::run(A,B,D,params,aa,bb,lane,sg,group,lid);
}}\n'''
 (b/'steel.metal').write_text(shader)
 lock=read(p.ROOT/'native/metal-proof/toolchain.json');metal=['xcrun','--toolchain',lock['metalToolchain'],'--sdk','macosx','metal']
 p.run([*metal,'-I',U,'-mmacosx-version-min=26.0','-std=metal3.2','-fno-fast-math','-ffp-contract=off','-c',b/'steel.metal','-o',b/'steel.air'])
 p.run([*metal,b/'steel.air','-o',b/'steel.metallib'])
 p.run([*metal,'-mmacosx-version-min=26.0','-std=metal3.2','-fno-fast-math','-ffp-contract=off','-c',F/'study.metal','-o',b/'own.air'])
 p.run([*metal,b/'own.air','-o',b/'own.metallib'])
 p.run([p.CLANG,'-std=c++23','-O2','-fobjc-arc','-fblocks',driver,'-framework','Foundation','-framework','Metal','-o',b/'study'])
 rng=random.Random(20261030)
 for shape in SHAPES:
  m,k,n=shape;values=[bits(rng.uniform(-1,1)) for _ in range(m*k+k*n)]
  (b/('input-'+'-'.join(map(str,shape)))).write_bytes(struct.pack('<'+'I'*len(values),*values))
 artifacts=[b/'steel.metal',b/'steel.metallib',b/'study',b/'own.metallib',*b.glob('input-*')]
 saved=dict(pin=PIN,signature=signature,artifacts={p.key(x):p.sha(x) for x in artifacts},commands=p.COMMANDS,tiles=TILES)
 manifest.write_text(json.dumps(saved,indent=2)+'\n');a.output.write_text(manifest.read_text());sys.exit(0)
saved=read(manifest);assert saved['signature']==signature and saved['pin']==PIN
for name,digest in saved['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
if a.phase=='verify':
 qpath=p.ROOT/'benchmarks/results/gpu-matrix-exact-model-2026-10-30.json';q=read(qpath);old=p.ROOT/'benchmarks/.build/gpu-matrix-exact-model-2026-10-30'
 checks=[];cases=[]
 for row in q['records']:
  if row['variant']=='baseline':cases.append(dict(name=row['name'],shape=row['shape'],input=old/(row['name']+'.input'),mode=row['mode'],old=True,input_sha256=row['input_sha256']))
 for shape in [(1,1,1),(3,7,5),(17,15,31),(33,17,65),(63,31,67),(65,33,63),(31,65,33),(97,513,35)]:
  m,k,n=shape;rng=random.Random(917+m+k+n)
  values=[bits(rng.uniform(-1,1)) for _ in range(m*k+k*n)]
  path=b/('tail-'+'-'.join(map(str,shape)));path.write_bytes(struct.pack('<'+'I'*len(values),*values))
  cases.append(dict(name=path.name,shape=shape,input=path,mode='bounded',old=False))
 for shape in SHAPES:
  path=b/('input-'+'-'.join(map(str,shape)));cases.append(dict(name=path.name,shape=shape,input=path,mode='bounded',old=False,performance=True))
 for case in cases:
  m,k,n=case['shape'];inputfile=case['input'];inputs=words(inputfile)
  if case.get('input_sha256'):assert p.sha(inputfile)==case['input_sha256']
  exact,absolute=oracle(inputs,case['shape']) if case['mode']!='special' else ([],[])
  variants=['baseline' if m%16==0 and n%16==0 and k>=512 and k%64==0 else 'guarded',*TILES]
  if m%32==0 and n%32==0 and k%32==0:variants.append('simd')
  for variant in variants:
   out=b/(case['name']+'-'+variant+'.output')
   rows=p.observations(p.run(command(variant,case['shape'],inputfile,1,1,1,out),timeout=300,
                        env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')),m*n,1,1)
   actual=words(out);assert len(actual)==m*n*3;assert actual[:m*n]==actual[m*n:2*m*n]==actual[2*m*n:];actual=actual[:m*n]
   record=dict(name=case['name'],shape=case['shape'],mode=case['mode'],variant=variant,
      input=p.key(inputfile),input_sha256=p.sha(inputfile),output=p.key(out),output_sha256=p.sha(out),
      classes={c:sum(category(w)==c for w in actual) for c in set(map(category,actual))},first_bits=f'{actual[0]:08x}',guards=True,inputs_unchanged=True,repeatable=True,resources=rows[0])
   if exact:
    record.update(assess(actual,exact,absolute,k))
    if case['mode']=='bounded':assert not record['nonfinite_cells'] and not record['bound_failures'],record
   if case['old']:
    for oldvariant in ('baseline','blocked'):
     reference=next(r for r in q['records'] if r['name']==case['name'] and r['variant']==oldvariant)
     path=p.ROOT/reference['output'];assert p.sha(path)==reference['output_sha256']
     previous=words(path)[:m*n]
     record['different_'+oldvariant]=sum(x!=y and not(category(x)==category(y)=='nan') for x,y in zip(actual,previous))
    if variant in ('baseline','simd'):assert record['different_'+('baseline' if variant=='baseline' else 'blocked')]==0,record
   checks.append(record)
  print(case['name'],'all applicable variants qualified',flush=True)
 data=dict(contract=__doc__,checks=checks,production_change=False,upstream=PIN,old_qualification_sha256=p.sha(qpath))
else:
 qpath=b/'qualification.json';q=read(qpath);assert q['checks']
 for flag in ('MTL_DEBUG_LAYER','MTL_SHADER_VALIDATION','DYLD_INSERT_LIBRARIES'):assert os.getenv(flag) in (None,'','0')
 selection=None
 if a.study in ('confirm','prototype','repeat'):
  assert a.selection;selection=read(a.selection);assert selection['study']=='screen' and selection['qualification_sha256']==p.sha(qpath)
  import statistics as st
  winners={shape:max(TILES,key=lambda name:st.median(x['speedup'] for x in selection['pairs'] if tuple(x['group'][:3])==shape and x['group'][3]==name)) for shape in SHAPES}
 def invoke(cell,count,rounds):
  side,m,k,n,tile=cell
  if side=='baseline':variant='simd' if a.study=='prototype' else 'baseline'
  else:variant='baseline' if a.study=='aa' else tile if a.study=='screen' else winners[(m,k,n)]
  return p.observations(p.run(command(variant,(m,k,n),b/f'input-{m}-{k}-{n}',count,rounds),timeout=600),m*n,count,rounds)
 cells=[[side,*shape,tile] for shape in SHAPES for tile in (TILES if a.study=='screen' else ['selected']) for side in ('baseline','current')]
 data=p.measure_paired(cells,invoke,lambda c:c[1]*c[3],blocks=3 if a.study in ('aa','screen') else 5,rounds=3,target_ms=100,adaptive=True,max_iterations=256)
 data.update(contract=__doc__,production_change=False,study=a.study,qualification_sha256=p.sha(qpath),upstream=PIN)
 if selection:data.update(selection_sha256=p.sha(a.selection),winners={'-'.join(map(str,s)):v for s,v in winners.items()})
assert read(manifest)['signature']==signature
p.evidence(a.output,data,sources+[manifest],[p.ROOT/x for x in saved['artifacts']],p.CLANG)
