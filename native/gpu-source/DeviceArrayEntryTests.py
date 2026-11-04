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
for typ in ('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64'):
    cases['typed-'+typ] = (f'''fn work(values:[{typ}],scale:{typ})->{typ}:
  total:{typ}=0
  for value in values:
    total=total+value*scale
  total
''', f'  values:[{typ}]=[1,2,3,4]\n', f'work(values,{typ}(2))', typ, 20)
cases['bool-input-result'] = ('''fn work(values:[Bool])->Bool:
  result=true
  for value in values:
    result=result and value
  result
''', '  values:[Bool]=[true,false,true]\n', 'work(values)', 'Bool', False)
cases['different-lengths-and-aliases'] = ('''fn work(left:[int],right:[int],alias:[int])->int:
  total=0
  for value in left:
    total=total+value
  for value in right:
    total=total+value
  for value in alias:
    total=total+value
  total
''', '  values:[int]=[1,2,3]\n  other:[int]=[10,20]\n',
    'work(values,other,values)', 'int', 42)
cases['copy-sort-keeps-input'] = ('''fn work(values:[int])->int:
  local=copy(values)
  sort(local)
  local[0]*100+values[0]
''', '  values:[int]=[9,2,5]\n', 'work(values)', 'int', 209)
sum_helper = '''fn work(values:[int])->int:
  total=0
  for value in values:
    total=total+value
  total
'''
cases['empty'] = (sum_helper, '  values:[int]=[]\n', 'work(values)', 'int', 0)
cases['multi-page'] = (sum_helper, '''  values=for i in range(17000):
    yield i
''', 'work(values)', 'int', 144491500)
for name,typ,literal,expected in (
    ('int64-min','int64','-9223372036854775808',-9223372036854775808),
    ('int64-max','int64','9223372036854775807',9223372036854775807),
    ('uint64-max','uint64','18446744073709551615',18446744073709551615),
    ('negative-int8','int8','-128',-128),
    ('negative-zero-f32','float32','-0.0',-0.0),
    ('negative-zero-f64','float64','-0.0',-0.0),
):
    cases[name]=(f'fn work(values:[{typ}])->{typ}:\n  values[0]\n',
                 f'  values:[{typ}]=[{literal}]\n','work(values)',typ,expected)
cases['captured-record-root-through-gc']=('''type Box:
  values:[int]
fn work(values:[int])->int:
  box=Box {values:values}
  total=0
  for i in range(1000):
    local=[i+1,0]
    total=total+local[0]
  total+box.values[0]
''','  values:[int]=[17]\n','work(values)','int',500517)
results=[]
for name,(helper,setup,call,typ,expected) in cases.items():
    root=a.root/name
    root.mkdir(exist_ok=True)
    source=root/'cpu.k'
    source.write_text(helper+f'fn main()->{typ}:\n'+setup+'  '+call+'\n')
    cpu=root/('cpu'+suffix)
    run([a.compiler,source,*flags,'-o',cpu])
    raw=run([cpu])
    value=(raw=='true') if typ=='Bool' else (float(raw) if typ.startswith('float') else int(raw))
    assert value==expected,(name,'CPU',raw,expected)
    if typ.startswith('float'):
        assert struct.pack('<d',value)==struct.pack('<d',expected),(name,'CPU float bits',raw)
    source=root/'gpu.k'
    source.write_text(helper+f'fn main()->{typ} ! TaskError:\n'+setup+
                      '  try parallel:\n    gpu=devices.gpu()\n    try await spawn '+call+' on gpu\n')
    bundle=root/'device'
    arithmetic='hardware-with-software-fallback' if a.backend=='metal' and typ=='float64' else 'hardware'
    run([a.compiler,source,'--target',a.backend,'--arithmetic',arithmetic,*flags,'-o',bundle])
    assert 'DeviceArrayInputJob<' in (bundle/'host.cpp').read_text(),name
    outputs=[]
    for workers in (() if a.compile_only else ('1','4')):
        output=run([bundle/('program'+suffix)],dict(os.environ,KALANDRA_WORKERS=workers))
        bits=int(output,16) if typ!='Bool' else None
        if typ=='Bool':
            assert output in ('true','false'),(name,'noncanonical Bool',output)
            value=output=='true'
        elif typ.startswith('float'):
            width=int(typ[5:])
            value=struct.unpack('<f' if width==32 else '<d',bits.to_bytes(width//8,'little'))[0]
        else:
            width=64 if typ=='int' else int(''.join(filter(str.isdigit,typ)))
            value=bits-(1<<width) if typ.startswith('int') and bits&(1<<(width-1)) else bits
        assert value==expected,(name,'GPU',workers,output,value,expected)
        if typ.startswith('float'):
            encoding='<f' if typ=='float32' else '<d'
            assert struct.pack(encoding,value)==struct.pack(encoding,expected),(name,'GPU float bits',output)
        outputs.append(output)
    results.append({'case':name,'expected':expected,'status':'compiled' if a.compile_only else 'executed','workers':[] if a.compile_only else [1,4],'raw':outputs})
    (a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,'results':results},indent=2)+'\n')
    print(name,'CPU/compile-only passed' if a.compile_only else 'CPU/GPU passed',flush=True)

# Array captures retain the existing read/alias and exclusive-mutation rules.
negative_helper='fn work(values:[int])->int:\n  values[0]\n'
negatives={
    'pending-write': negative_helper+'''fn main()->int ! TaskError:
  values:[int]=[1,2,3]
  try parallel:
    gpu=devices.gpu()
    pending=spawn work(values) on gpu
    values[0]=9
    try await pending
''',
    'pending-alias-write': negative_helper+'''fn main()->int ! TaskError:
  values:[int]=[1,2,3]
  alias=values
  try parallel:
    gpu=devices.gpu()
    pending=spawn work(values) on gpu
    sort(alias)
    try await pending
''',
    'spawn-mut': '''fn work(mut values:[int])->int:
  values[0]=9
  values[0]
fn main()->int ! TaskError:
  values:[int]=[1,2,3]
  try parallel:
    gpu=devices.gpu()
    try await spawn work(values) on gpu
'''
}
for name,text in negatives.items():
    source=a.root/(name+'.k')
    source.write_text(text)
    r=subprocess.run([str(a.compiler),str(source),'--target',a.backend,*flags,
                      '-o',str(a.root/name)],capture_output=True,text=True,timeout=240)
    (a.root/(name+'.log')).write_text(r.stdout+r.stderr)
    assert r.returncode!=0 and 'S077' in r.stderr,(name,r.returncode,r.stdout,r.stderr)
    results.append({'case':name,'status':'rejected','diagnostic':'S077'})
    print(name,'rejected S077',flush=True)
(a.root/'report.json').write_text(json.dumps({'backend':a.backend,'variant':a.variant,
                                           'complete':True,'results':results},indent=2)+'\n')
