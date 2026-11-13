"""Check typed host-array captures against CPU and explicit scalar oracles.

This suite executes complete source programs. It does not claim coverage for
array results, aggregate entries, or Windows GPU hardware.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--variant', choices=('debug', 'optimized'), default='optimized')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
flags = ['-O0' if a.variant == 'debug' else '-O2']
suffix = '.exe' if os.name == 'nt' else ''

def run(command, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                       timeout=240, env=env)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout.strip()

cases = {}
for typ in ('i8','i16','i32','i64','u8','u16','u32','u64','f32','f64'):
    cases['typed-'+typ] = (f'''fn work(values:[{typ}],scale:{typ})->{typ}:
  total:{typ}=0
  for value in values:
    total=total+value*scale
  return total
''', f'  values:[{typ}]=[1,2,3,4]\n', f'work(values,{typ}(2))', typ, 20)
cases['bool-input-result'] = ('''fn work(values:[bool])->bool:
  result=true
  for value in values:
    result=result and value
  return result
''', '  values:[bool]=[true,false,true]\n', 'work(values)', 'bool', False)
cases['different-lengths-and-aliases'] = ('''fn work(left:[i64],right:[i64],alias:[i64])->i64:
  total=0
  for value in left:
    total=total+value
  for value in right:
    total=total+value
  for value in alias:
    total=total+value
  return total
''', '  values:[i64]=[1,2,3]\n  other:[i64]=[10,20]\n',
    'work(values,other,values)', 'i64', 42)
cases['copy-sort-keeps-input'] = ('''fn work(values:[i64])->i64:
  local=copy(values)
  sort(local)
  return local[0]*100+values[0]
''', '  values:[i64]=[9,2,5]\n', 'work(values)', 'i64', 209)
sum_helper = '''fn work(values:[i64])->i64:
  total=0
  for value in values:
    total=total+value
  return total
'''
cases['empty'] = (sum_helper, '  values:[i64]=[]\n', 'work(values)', 'i64', 0)
cases['multi-page'] = (sum_helper, '''  values=array(for i in range(17000):
    yield i
  )
''', 'work(values)', 'i64', 144491500)
for name,typ,literal,expected in (
    ('int64-min','i64','-9223372036854775808',-9223372036854775808),
    ('int64-max','i64','9223372036854775807',9223372036854775807),
    ('uint64-max','u64','18446744073709551615',18446744073709551615),
    ('negative-int8','i8','-128',-128),
    ('negative-zero-f32','f32','-0.0',-0.0),
    ('negative-zero-f64','f64','-0.0',-0.0),
):
    cases[name]=(f'fn work(values:[{typ}])->{typ}:\n  return values[0]\n',
                 f'  values:[{typ}]=[{literal}]\n','work(values)',typ,expected)
cases['captured-record-root-through-gc']=('''type Box:
  values:[i64]
fn work(values:[i64])->i64:
  box=Box {values:values}
  total=0
  for i in range(1000):
    local=[i+1,0]
    total=total+local[0]
  return total+box.values[0]
''','  values:[i64]=[17]\n','work(values)','i64',500517)
results=[]
for name,(helper,setup,call,typ,expected) in cases.items():
    root=a.root/name
    root.mkdir(exist_ok=True)
    source=root/'cpu.k'
    source.write_text(helper+f'fn main()->{typ}:\n'+setup+'  return '+call+'\n')
    cpu=root/('cpu'+suffix)
    run([a.compiler, '--test-entry', 'main',source,*flags,'-o',cpu])
    raw=run([cpu])
    value=(raw=='true') if typ=='bool' else (float(raw) if typ.startswith('f') else int(raw))
    assert value==expected,(name,'CPU',raw,expected)
    if typ.startswith('f'):
        assert struct.pack('<d',value)==struct.pack('<d',expected),(name,'CPU float bits',raw)
    source=root/'gpu.k'
    source.write_text(helper+f'fn main()->{typ} ! TaskError:\n'+setup+
                      '  return try parallel:\n    gpu=devices.gpu()\n    try await spawn '+call+' on gpu\n')
    bundle=root/'device'
    arithmetic='hardware-with-software-fallback' if a.backend=='metal' and typ=='f64' else 'hardware'
    run([a.compiler, '--test-entry', 'main',source,'--target',a.backend,'--arithmetic',arithmetic,*flags,'-o',bundle])
    assert 'DeviceArrayInputJob<' in (bundle/'host.cpp').read_text(),name
    outputs=[]
    for workers in (() if a.compile_only else ('1','4')):
        output=run([bundle/('program'+suffix)],dict(os.environ,KALANDRA_WORKERS=workers))
        bits=int(output,16) if typ!='bool' else None
        if typ=='bool':
            assert output in ('true','false'),(name,'noncanonical Bool',output)
            value=output=='true'
        elif typ.startswith('f'):
            width=int(typ[1:])
            value=struct.unpack('<f' if width==32 else '<d',bits.to_bytes(width//8,'little'))[0]
        else:
            width=64 if typ=='i64' else int(''.join(filter(str.isdigit,typ)))
            value=bits-(1<<width) if typ.startswith('i') and bits&(1<<(width-1)) else bits
        assert value==expected,(name,'GPU',workers,output,value,expected)
        if typ.startswith('f'):
            encoding='<f' if typ=='f32' else '<d'
            assert struct.pack(encoding,value)==struct.pack(encoding,expected),(name,'GPU float bits',output)
        outputs.append(output)
    results.append({'case':name,'expected':expected,'status':'compiled' if a.compile_only else 'executed','workers':[] if a.compile_only else [1,4],'raw':outputs})
    (a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,'results':results},indent=2)+'\n')
    print(name,'CPU/compile-only passed' if a.compile_only else 'CPU/GPU passed',flush=True)

# Array captures retain the existing read/alias and exclusive-mutation rules.
negative_helper='fn work(values:[i64])->i64:\n  return values[0]\n'
negatives={
    'pending-write': negative_helper+'''fn main()->i64 ! TaskError:
  values:[i64]=[1,2,3]
  return try parallel:
    gpu=devices.gpu()
    pending=spawn work(values) on gpu
    values[0]=9
    try await pending
''',
    'pending-alias-write': negative_helper+'''fn main()->i64 ! TaskError:
  values:[i64]=[1,2,3]
  alias=values
  return try parallel:
    gpu=devices.gpu()
    pending=spawn work(values) on gpu
    sort(alias)
    try await pending
''',
    'spawn-mut': '''fn work(mut values:[i64])->i64:
  values[0]=9
  return values[0]
fn main()->i64 ! TaskError:
  values:[i64]=[1,2,3]
  return try parallel:
    gpu=devices.gpu()
    try await spawn work(values) on gpu
'''
}
for name,text in negatives.items():
    source=a.root/(name+'.k')
    source.write_text(text)
    r=subprocess.run([str(a.compiler), '--test-entry', 'main',str(source),'--target',a.backend,*flags,
                      '-o',str(a.root/name)],capture_output=True,text=True,timeout=240)
    (a.root/(name+'.log')).write_text(r.stdout+r.stderr)
    assert r.returncode!=0 and 'S077' in r.stderr,(name,r.returncode,r.stdout,r.stderr)
    results.append({'case':name,'status':'rejected','diagnostic':'S077'})
    print(name,'rejected S077',flush=True)
(a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,
                                           'complete':True,'results':results},indent=2)+'\n')
