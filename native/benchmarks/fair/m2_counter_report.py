"""Decode Instruments M2 counter samples with target-only interval controls.

Instruments counter values are device-wide, not per-process. Restrict to interiors
of the 20 measured compute batches and separately exclude overlapping GPU work.
All statistics are sample means, not percentages of whole-program elapsed time.
"""
import bisect,collections,csv,hashlib,json,re,shutil,statistics as st,xml.etree.ElementTree as E
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];B=ROOT/'benchmarks/.build/gpu-m2-counters-2026-10-29';OUT=ROOT/'benchmarks/results'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
def resolve(e,refs):
 if 'ref' in e.attrib:return refs[e.attrib['ref']]
 result=(e.text,e.attrib.get('fmt',e.text))
 if 'id' in e.attrib:refs[e.attrib['id']]=result
 return result
def rows(file):
 refs={};node=None
 for event,e in E.iterparse(file,events=('start','end')):
  if event=='start' and e.tag=='node':node=e.attrib['xpath']
  if event!='end' or e.tag!='row':continue
  for child in e.iter():
   if child is not e:resolve(child,refs)
  yield node,[resolve(c,refs) for c in e],list(e)
  e.clear()
toc=E.parse(B/'toc.xml').getroot();tables=toc.find('run/data').findall('table')
value_index=next(i for i,t in enumerate(tables,1) if t.get('schema')=='gpu-counter-value')
pid=toc.find('run/info/target/process')
if pid is None:pid=next(toc.iter('process'))
name=f"{pid.attrib['name']} ({pid.attrib['pid']})"
intervals=[]
for _,v,cols in rows(B/'gpu-intervals.xml'):
 process=next(value[1] for value,c in zip(v,cols) if c.tag=='process')
 intervals.append(dict(start=int(v[0][0]),end=int(v[0][0])+int(v[1][0]),channel=v[2][1],process=process))
long=sorted([r for r in intervals if r['process']==name and r['channel']=='Compute' and r['end']-r['start']>1000000],key=lambda r:r['start'])
log=[json.loads(x) for x in (B/'baseline-driver.jsonl').read_text().splitlines()]
assert len(long)==len(log)==22
for r,l in zip(long,log):assert abs(r['end']-r['start']-l['gpu_ns'])<100
windows=[(r['start']+100000,r['end']-100000) for r in long[2:]]
other=sorted((r['start']-20000,r['end']+20000) for r in intervals if r['process']!=name)
merged=[]
for start,end in other:
 if merged and start<=merged[-1][1]:merged[-1]=(merged[-1][0],max(end,merged[-1][1]))
 else:merged.append((start,end))
def contains(windows,t):
 i=bisect.bisect_right(windows,(t,float('inf')))-1
 return i if i>=0 and t<windows[i][1] else -1
names={};catalog=[]
for _,v,_ in rows(B/'counter-info.xml'):
 c=int(v[1][0]);names[c]=v[2][0];catalog.append(dict(id=c,name=v[2][0],description=v[5][0],unit=v[7][0],sample_interval=int(v[10][0])))
acc=collections.defaultdict(lambda:[0,0.,float('inf'),-float('inf')]);per=collections.defaultdict(lambda:[0,0.]);total=0
for node,v,_ in rows(B/'values.xml'):
 if not node.endswith(f'table[{value_index}]'):continue
 total+=1;t=int(v[0][0]);index=contains(windows,t)
 if index<0:continue
 counter=int(v[1][0]);value=float(v[2][0]);scopes=['target_batch']
 if contains(merged,t)<0:scopes.append('target_without_other_gpu')
 for scope in scopes:
  a=acc[(scope,counter)];a[0]+=1;a[1]+=value;a[2]=min(a[2],value);a[3]=max(a[3],value)
  a=per[(scope,counter,index)];a[0]+=1;a[1]+=value
summary=[]
for (scope,c),a in sorted(acc.items()):
 batches=[v[1]/v[0] for (s,k,i),v in per.items() if s==scope and k==c]
 summary.append(dict(scope=scope,name=names[c],samples=a[0],sample_mean=a[1]/a[0],minimum=a[2],maximum=a[3],batch_mean_median=st.median(batches),batch_mean_range=[min(batches),max(batches)]))
assert all(r['samples']>0 for r in summary);assert len(summary)==62
# The following compiler fields were transcribed from Xcode's Shaders table and
# Pipeline State sidebar, not inferred from MSL source or from the CSV counters.
compiler=[dict(shader='kalandra_matmul_hardware_register16',allocated_registers=100,high_register=100,spilled_bytes=0,temporary_registers=52,max_theoretical_occupancy_percent=29.167,wait_memory_instruction_cost_percent=51.003),dict(shader='kalandra_matmul_hardware_reuse_quad',allocated_registers=254,high_register=254,spilled_bytes=592,temporary_registers=128,max_theoretical_occupancy_percent=12.5,wait_memory_instruction_cost_percent=63.463)]
exports={}
for variant in ('baseline','quad'):
 file=B/f'{variant}-counters.csv';dest=OUT/f'gpu-m2-{variant}-counters-2026-10-29.csv';shutil.copyfile(file,dest)
 # Preserve duplicate header names in the raw export; selected report fields are unique.
 records=list(csv.reader(file.open()));assert len(records[0])==len(records[1]);assert not any(any(r) for r in records[2:])
 exports[variant]=dict(zip(records[0],records[1]))
files=[Path(__file__),B/'toc.xml',B/'counter-info.xml',B/'values.xml',B/'gpu-intervals.xml',B/'baseline-driver.jsonl',B/'quad-capture.log',B/'public-counters.json',B/'Counters.mm',B/'baseline-counters.csv',B/'quad-counters.csv',ROOT/'benchmarks/.build/gpu-resource-probe-2026-10-29/Driver.mm',ROOT/'benchmarks/.build/gpu-resource-probe-2026-10-29/capture',ROOT/'benchmarks/.build/gpu-output-reuse-2026-10-29/quad/study.metallib']
# A reproducible content manifest for the small GPU captures; raw traces stay ignored.
for trace in (ROOT/'benchmarks/.build/gpu-resource-probe-2026-10-29/baseline.gputrace',B/'quad.gputrace'):
 files.extend(f for f in trace.rglob('*') if f.is_file())
result=dict(acceptance=False,production_change=False,device='Apple M2 Max',shape=[64,2048,512],counter_catalog=catalog,total_counter_rows=total,target_process=name,measured_batches=20,warmup_batches_excluded=2,batch_edge_trim_ns=100000,other_gpu_edge_margin_ns=20000,method=__doc__,compiler_observation_source='Xcode 27.1 GUI Shaders table and Pipeline State sidebar, manually transcribed; allocated and temporary registers are distinct displayed fields.',compiler=compiler,xcode_encoder_counters=exports,instruments=summary,public_counters=json.loads((B/'public-counters.json').read_text()),evidence_sha256={str(f.relative_to(ROOT)):sha(f) for f in files},limitations=['One isolated FFN contraction shape; not whole-model acceptance.','Xcode full replay counters and instruction sampling alter timings; do not use replay durations as speedup benchmarks.','Instruments bandwidth/cache/MMU counters that remain zero are not evidence of zero traffic; Xcode replay reports nonzero values.','Memory-wait instruction cost is not a fraction of application wall time.','The quad changes several resource demands; these data do not isolate register allocation as its only cause.'])
file=OUT/'gpu-m2-resources-2026-10-29.json';file.write_text(json.dumps(result,indent=2)+'\n')
print(file)
for row in summary:
 if row['scope']=='target_without_other_gpu' and row['name'] in ('Compute Occupancy','ALU Limiter','ALU Utilization','F32 Utilization','Threadgroup/Imageblock Load Limiter','Buffer Read Limiter'):print(row['name'],round(row['sample_mean'],3),row['samples'])
