"""Check Bool GPU source results against CPU, preserving real Bool task identity."""
import argparse
import json
import os
import subprocess
from pathlib import Path
from DeviceBoolCases import cases

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--backend',choices=('metal','vulkan','cuda'),required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),default='optimized')
p.add_argument('--compile-only',action='store_true')
a=p.parse_args()
a.compiler=a.compiler.resolve()
a.root.mkdir(parents=True,exist_ok=True)
flags={'debug':['-O0'],'optimized':['-O2'],'sanitized':['--sanitize'],'thread-sanitized':['--thread-sanitize']}[a.variant]
suffix='.exe' if os.name=='nt' else ''
def run(command,env=None):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=240,env=env)
    assert result.returncode==0,(command,result.returncode,result.stdout,result.stderr)
    return result.stdout.strip()
results=[]
for name,cpu,gpu,expected,typ in cases():
    root=a.root/name;root.mkdir(exist_ok=True)
    source=root/'cpu.k';source.write_text(cpu)
    run([a.compiler,source,*flags,'-o',root/('cpu'+suffix)])
    if expected is None:
        r=subprocess.run([str(root/('cpu'+suffix))],capture_output=True,text=True,timeout=30)
        assert r.returncode==86 and not r.stdout and 'ArithmeticFault' in r.stderr,(name,r)
    else:
        text=str(expected).lower()
        assert run([root/('cpu'+suffix)])==text,(name,'CPU',text)
    source=root/'gpu.k';source.write_text(gpu)
    bundle=root/'device'
    run([a.compiler,source,'--target',a.backend,*flags,'-o',bundle])
    if not a.compile_only:
        for workers in ('1','4'):
            env=dict(os.environ,KALANDRA_WORKERS=workers)
            if expected is None:
                r=subprocess.run([str(bundle/('program'+suffix))],capture_output=True,text=True,timeout=30,env=env)
                assert r.returncode and not r.stdout and '/4' in r.stderr,(name,r)
            else:
                text=str(expected).lower() if typ=='bool' else format(expected,'016x')
                assert run([bundle/('program'+suffix)],env)==text,(name,'GPU',text)
    results.append({'case':name,'expected':expected,'resultType':typ,'status':'compiled' if a.compile_only else 'checked-fault' if expected is None else 'executed'})
    print(name,results[-1]['status'],flush=True)
# Returning Bool cannot hide unsupported arithmetic or change IEEE comparisons.
for label,value,expected in [('positive','f64(7)',True),('negative','f64(-7)',False),
                             ('zero','f64(0)',False),('nan','f64(0)/f64(0)',False)]:
    helper='fn predicate(value:f64)->bool:\n  return value>f64(0)\n'
    cpu_source=a.root/('float64-'+label+'-cpu.k')
    cpu_source.write_text(helper+'fn main()->bool:\n  return predicate('+value+')\n')
    cpu_binary=a.root/('float64-'+label+'-cpu'+suffix)
    run([a.compiler,cpu_source,*flags,'-o',cpu_binary])
    assert run([cpu_binary])==str(expected).lower()
    source=a.root/('float64-'+label+'.k')
    source.write_text(helper+'fn main()->bool ! TaskError:\n  return try parallel:\n    gpu=devices.gpu()\n'
                      '    try await spawn predicate('+value+') on gpu\n')
    policy='hardware'
    if a.backend=='metal':
        rejected=subprocess.run([str(a.compiler),str(source),'--target','metal','--arithmetic','hardware',*flags,'-o',str(a.root/('double-rejected-'+label))],capture_output=True,text=True,timeout=120)
        assert rejected.returncode and 'Metal has no hardware float64 arithmetic' in rejected.stderr,rejected
        policy='hardware-with-software-fallback'
    bundle=a.root/('float64-'+label)
    run([a.compiler,source,'--target',a.backend,'--arithmetic',policy,*flags,'-o',bundle])
    if not a.compile_only:
        assert run([bundle/('program'+suffix)])==str(expected).lower()
    results.append({'case':'float64-predicate-'+label,'expected':expected,'arithmetic':policy,
                    'status':'compiled' if a.compile_only else 'executed'})
for name,body in [('byte-as-bool','work(u8(1))'),('bool-as-byte','byte(true)')]:
    source=a.root/(name+'.k')
    source.write_text(f'fn work(flag:bool)->bool:\n  return flag\nfn byte(n:u8)->u8:\n  return n\nfn main()->i64 ! TaskError:\n  return try parallel:\n    gpu=devices.gpu()\n    result=try await spawn {body} on gpu\n    42\n')
    r=subprocess.run([str(a.compiler),str(source),'--target',a.backend,*flags,'-o',str(a.root/name)],capture_output=True,text=True,timeout=60)
    assert r.returncode and 'bool' in r.stderr and 'u8' in r.stderr,(name,r)
    results.append({'case':name,'status':'type-rejected'})
(a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,
    'gpuExecution':not a.compile_only,'results':results},indent=2)+'\n')
