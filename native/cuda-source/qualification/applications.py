"""Explicit physical CUDA complete-source correctness; no performance timing.

Uses existing compiler/SDK, installs nothing, and retains every build/run result.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--compiler',type=Path,help='Exercise the public --target cuda command instead of the private builder')
mode=p.add_mutually_exclusive_group(required=True)
mode.add_argument('--execute',action='store_true')
mode.add_argument('--compile-only',action='store_true')
a=p.parse_args();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[3]
base=(repo/'native/metal-source/placed.k').read_text()
cases=[]
for numeric in ('int8','uint8','int16','uint16','int32','uint32','int64','uint64','float32','float64'):
    source=a.output/(numeric+'.k');source.write_text(base.replace('float32',numeric))
    cases.append((numeric,source,numeric,[5,12,21],True))
for name,values,tensor in [('cpu-spawn',[52,54,64,66],True),('matmul',[163],False),('reduction',[31],False),
                         ('recovery',[180],False),('tensor-feedback',[.5,1,1.5],True),
                         ('scalar-control',[41/246,82/246,123/246],True)]:
    cases.append((name,repo/'native/metal-source'/f'{name}.k','float32',values,tensor))
cases.append(('checked-fault',Path(__file__).with_name('checked-fault.k'),'int8',[42],True))
records=[];failures=[];builds=[]
for policy in ('hardware','software','hardware-with-software-fallback'):
  for name,source,numeric,values,tensor in cases:
    out=a.output/policy/name;out.parent.mkdir(parents=True,exist_ok=True)
    command=[sys.executable,Path(__file__).with_name('build_application.py'),'--build',a.build,'--source',source,'--output',out,'--arithmetic',policy]
    if a.compiler:
        command=[a.compiler.resolve(),'--target','cuda','--arithmetic',policy,'-O2',source,'-o',out]
    built=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=240)
    (out.parent/(name+'-build.log')).write_text(built.stdout+built.stderr)
    builds.append({'case':name,'policy':policy,'status':built.returncode})
    if built.returncode:
      record={'case':name,'policy':policy,'stage':'build','status':built.returncode};records.append(record);failures.append(record)
    else:
      width=32 if numeric=='float32' else 64 if numeric=='float64' else int(numeric.removeprefix('u').removeprefix('int'))
      def bits(value):
        return int.from_bytes(struct.pack('<f' if width==32 else '<d',value),'little') if numeric.startswith('float') else value
      payload=','.join(f'{bits(v):0{width//4}x}' for v in values)
      expected=('['+payload+']' if tensor else payload)+'\n'
      for workers in ((1,2,4) if a.execute else ()):
        run=subprocess.run([out/'program'],capture_output=True,text=True,timeout=60,env=os.environ|{'KALANDRA_WORKERS':str(workers)})
        record={'case':name,'policy':policy,'workers':workers,'status':run.returncode,'stdout':run.stdout,'stderr':run.stderr,'expected':expected}
        records.append(record)
        if run.returncode or run.stderr or run.stdout!=expected:failures.append(record)
    (a.output/'results.json').write_text(json.dumps({'builds':builds,'records':records,'failures':failures},indent=2)+'\n')
    print(('FAIL' if any(r['case']==name and r['policy']==policy for r in failures) else 'PASS')+': '+name+' '+policy,flush=True)
if failures:raise SystemExit(f'{len(failures)} failures retained in results.json')
print(f'PASS: {len(builds)} source builds, {len(records)} execution records; all ten types and three policies; GPU execution={a.execute}')
