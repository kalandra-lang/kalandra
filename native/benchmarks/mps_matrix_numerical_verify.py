"""Compare MPSGraph default and explicitly full precision on frozen edge cases.

Correctness-only numerical-policy investigation. Production has no MPS dependency.
"""
import argparse,json,math,os,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();b=a.build.resolve();assert not b.exists() and not a.output.exists();b.mkdir(parents=True)
source=Path(__file__).with_name('MpsMatrixNumericalProbe.mm')
qpath=p.ROOT/'benchmarks/results/gpu-matrix-exact-model-2026-10-30.json'
q=json.loads(qpath.read_text());old=p.ROOT/'benchmarks/.build/gpu-matrix-exact-model-2026-10-30'
p.run([p.CLANG,'-std=c++23','-O2','-mmacosx-version-min=26.0','-fobjc-arc','-fblocks',source,
       '-framework','Foundation','-framework','Metal','-framework','MetalPerformanceShaders',
       '-framework','MetalPerformanceShadersGraph','-o',b/'probe'])
def category(word):
 if word&0x7f800000==0x7f800000:return 'nan' if word&0x7fffff else '-inf' if word>>31 else '+inf'
 return ('-zero' if word>>31 else '+zero') if word&0x7fffffff==0 else 'finite'
def integer(word):
 e=(word>>23)&255;assert e!=255
 value=(word&0x7fffff) if not e else ((word&0x7fffff)|0x800000)<<(e-1)
 return -value if word>>31 else value
records=[]
for reference in (r for r in q['records'] if r['variant']=='baseline'):
 name=reference['name'];m,k,n=reference['shape'];inputfile=old/(name+'.input')
 assert p.sha(inputfile)==reference['input_sha256']
 inputs=struct.unpack('<'+'I'*(m*k+k*n),inputfile.read_bytes())
 exact=[];absolute=[]
 if reference['mode']!='special':
  av=list(map(integer,inputs[:m*k]));bv=list(map(integer,inputs[m*k:]));cache={}
  cols=[tuple(bv[i*n+c] for i in range(k)) for c in range(n)]
  for r in range(m):
   row=tuple(av[r*k:(r+1)*k])
   for col in cols:
    key=(row,col)
    if key not in cache:
     products=[x*y for x,y in zip(row,col)];cache[key]=(sum(products),sum(map(abs,products)))
    x,total=cache[key];exact.append(x);absolute.append(total)
 previous={}
 for variant in ('baseline','naive','blocked'):
  r=next(r for r in q['records'] if r['name']==name and r['variant']==variant)
  path=p.ROOT/r['output'];assert p.sha(path)==r['output_sha256']
  previous[variant]=struct.unpack('<'+'I'*(m*n*3),path.read_bytes())[:m*n]
 for policy in ('default','full'):
  out=b/(name+'-'+policy+'.output')
  try:
   status=json.loads(p.run([b/'probe',m,k,n,inputfile,out,policy],timeout=180,
                    env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')))
  except RuntimeError as error:
   records.append(dict(name=name,shape=[m,k,n],policy=policy,failed=True,error=str(error),input_sha256=p.sha(inputfile)))
   print(name,policy,'FAILED; retained diagnostic',flush=True)
   continue
  assert status==dict(rounds=3,cells=m*n,default_reduced_precision=0,inputs_unchanged=True)
  words=struct.unpack('<'+'I'*(m*n*3),out.read_bytes());assert words[:m*n]==words[m*n:2*m*n]==words[2*m*n:];words=words[:m*n]
  same=lambda x,y:x==y or category(x)==category(y)=='nan'
  row=dict(name=name,shape=[m,k,n],policy=policy,classes={c:sum(category(w)==c for w in words) for c in set(map(category,words))},
           first_bits=f'{words[0]:08x}',differences={v:sum(not same(x,y) for x,y in zip(words,previous[v])) for v in previous},
           input_sha256=p.sha(inputfile),output_sha256=p.sha(out),output=p.key(out),status=status)
  if exact:
   indices=[i for i,w in enumerate(words) if category(w) in ('finite','+zero','-zero')]
   errors=[abs((integer(words[i])<<149)-exact[i]) for i in indices]
   row.update(nonfinite_cells=m*n-len(indices),
              bound_failures=sum(err*((1<<24)-k)>k*absolute[i] for i,err in zip(indices,errors)),
              max_relative_error=max((err/abs(exact[i]) for i,err in zip(indices,errors) if exact[i]),default=None),
              max_absolute_error=math.ldexp(float(max(errors)),-298) if errors else None)
  records.append(row)
  print(name,policy,row['first_bits'],row['classes'],row['differences'],flush=True)
data=dict(contract=__doc__,production_change=False,performance_acceptance=False,records=records,
          qualification_sha256=p.sha(qpath),cases=29,limitations='Observed on this device and shapes, not a universal MPSGraph reduction-order guarantee.')
p.evidence(a.output,data,[Path(__file__),source,Path(p.__file__)],[b/'probe'],p.CLANG)
