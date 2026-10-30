"""Diagnostic GPU interval / reported DVFS-state correlation; not acceptance.

A timestamp-only derivative of the frozen same-runtime driver is sampled by a
rootless temporary macmon helper. Sampling overlaps profiling intentionally;
these results must not be substituted for uninstrumented acceptance timings.
"""
import argparse,json,os,subprocess,sys,time,struct,statistics as st
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent/'fair'))
import protocol as p
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--phase',choices=('prepare','profile'),required=True)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();b=a.build.resolve();assert not a.output.exists()
f=p.ROOT/'benchmarks/.build/gpu-upload-cache-atomic-2026-10-29';old=p.ROOT/'benchmarks/.build/gpu-private-inprocess-v2-2026-10-29'
manifest=b/'manifest.json';helper=Path('/private/tmp/kalandra-macmon-target/release/examples/kalandra_telemetry')
sources=[Path(__file__),p.ROOT/'native/benchmarks/GpuPowerTelemetry.rs',old/'manifest.json']
sig={p.key(x.resolve()):p.sha(x) for x in sources}
if a.phase=='prepare':
 b.mkdir(parents=True,exist_ok=False)
 frozen=json.loads((old/'manifest.json').read_text())
 for field in ('signature','artifacts'):
  for name,digest in frozen[field].items():assert p.sha(p.ROOT/name)==digest,name
 body=(old/'Driver.cpp').read_text()
 needle='        auto start = std::chrono::steady_clock::now();';assert body.count(needle)==1
 body=body.replace(needle,'        const auto utcStart = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();\n'+needle)
 needle='        const double ns = ffn::elapsed(start);';assert body.count(needle)==1
 body=body.replace(needle,needle+'''
        const auto utcEnd = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::printf("{\\\"interval\\\":true,\\\"round\\\":%d,\\\"start_ns\\\":%lld,\\\"end_ns\\\":%lld}\\n",round,(long long)utcStart,(long long)utcEnd);''')
 (b/'Driver.cpp').write_text(body)
 p.run([p.CLANG,'-std=c++23','-O2','-pthread','-ffp-contract=off','-mmacosx-version-min=26.0','-I'+str(f),'-I'+str(p.ROOT/'native/metal'),'-I'+str(p.ROOT/'native/benchmarks/fair'),b/'Driver.cpp',old/'metal.o',f/'candidate-task.a','-framework','Foundation','-framework','Metal','-o',b/'gpu'])
 checks=[]
 for pattern in (0,1):
  def output(exe):return json.loads(p.run([exe,f/'bundle',64,512,1,1,0,1,pattern,2,1]),parse_int=lambda n:-0.0 if n=='-0' else int(n))
  bits=lambda row:struct.pack('<'+'f'*len(row['cells']),*row['cells'])
  assert bits(output(b/'gpu'))==bits(output(old/'gpu'));checks.append(dict(pattern=pattern,passed=True))
 for src,name in [(helper,'telemetry'),(Path('/private/tmp/kalandra-macmon/LICENSE'),'macmon-LICENSE'),(Path('/private/tmp/kalandra-macmon/Cargo.lock'),'macmon-Cargo.lock')]:
  import shutil
  shutil.copyfile(src,b/name)
  if name=='telemetry':(b/name).chmod(src.stat().st_mode)
 files=[b/'Driver.cpp',b/'gpu',b/'telemetry',b/'macmon-LICENSE',b/'macmon-Cargo.lock',old/'metal.o',f/'candidate-task.a']
 manifest.write_text(json.dumps(dict(signature=sig,artifacts={p.key(x):p.sha(x) for x in files},checks=checks,commands=p.COMMANDS,telemetry_source='https://github.com/vladkens/macmon',telemetry_revision=subprocess.check_output(['git','-C','/private/tmp/kalandra-macmon','rev-parse','HEAD'],text=True).strip()),indent=2)+'\n')
 a.output.write_text(manifest.read_text());sys.exit(0)
frozen=json.loads(manifest.read_text());assert frozen['signature']==sig
for name,digest in frozen['artifacts'].items():assert p.sha(p.ROOT/name)==digest,name
assert len(frozen['checks'])==2 and all(x['passed'] for x in frozen['checks'])
with (b/'telemetry.jsonl').open('w') as samplelog:
 sampler=subprocess.Popen([str(b/'telemetry'),'180'],stdout=samplelog,stderr=(b/'telemetry-stderr.txt').open('w'))
 try:
  for _ in range(150):
   if '\n' in (b/'telemetry.jsonl').read_text():break
   if sampler.poll() is not None:raise RuntimeError((b/'telemetry-stderr.txt').read_text())
   time.sleep(.1)
  else:raise RuntimeError('No first telemetry sample within 15 seconds')
  raw=p.run([b/'gpu',f/'bundle',64,512,1024,20,0,0,0,2,1],env=dict(os.environ,KALANDRA_PAIR_CONTROL='1'))
  time.sleep(1.2)
 finally:
  sampler.terminate();sampler.wait(timeout=10)
rows=[json.loads(x) for x in raw.splitlines()];samples=[json.loads(x) for x in (b/'telemetry.jsonl').read_text().splitlines()]
intervals={r['round']:r for r in rows if r.get('interval')};timed=[r for r in rows if 'ns'in r];assert len(intervals)==len(timed)==22
joined=[]
for r in timed:
 if r['round']<0:continue
 interval=intervals[r['round']];start,end=interval['start_ns'],interval['end_ns'];weights=[(max(0,min(end,x['end_ns'])-max(start,x['start_ns'])),x) for x in samples]
 total=sum(w for w,x in weights);covered=total/(end-start)
 weighted=lambda key:sum(w*x[key] for w,x in weights)/total if total else None
 joined.append(dict(round=r['round'],wall_ns=r['ns']/1024,gpu_ns=r['gpu_ns']/1024,encoding_ns=r['encoding_ns']/1024,coverage=covered,gpu_freq_mhz=weighted('gpu_freq_mhz'),gpu_active_ratio=weighted('gpu_active_ratio'),gpu_power_w=weighted('gpu_power_w')))
print('Matched intervals',len(joined),'minimum coverage',round(min(x['coverage'] for x in joined),3),'reported GPU MHz range',min(x['gpu_freq_mhz'] or 0 for x in joined),max(x['gpu_freq_mhz'] or 0 for x in joined),flush=True)
p.evidence(a.output,dict(contract=__doc__,raw_intervals=rows,raw_samples=samples,joined=joined,telemetry_revision=frozen['telemetry_revision'],interpretation='Reported DVFS residency frequency is not an independent instantaneous hardware-clock measurement; temporal correlation cannot establish sole causality.'),sources+[manifest],[p.ROOT/x for x in frozen['artifacts']],p.ROOT/'build/native/native/kalandra')
