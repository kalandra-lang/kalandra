"""Source-level GPU array return correctness, alias identity and ownership diagnostics."""
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
p.add_argument('--case', action='append', default=[])
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
flags = ['-O0' if a.variant == 'debug' else '-O2']
suffix = '.exe' if os.name == 'nt' else ''

def run(command, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240, env=env)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout.strip()

cases = {}
for typ in ('int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64', 'float32', 'float64', 'Bool'):
    for alias in (False, True):
        boolean = typ == 'Bool'
        initial = 'true,false,true' if boolean else '1,2,3'
        changed = 'false' if boolean else '9'
        expected = (False if alias else True) if boolean else (9 if alias else 1)
        body = 'values' if alias else 'copy(values)'
        cases[('alias-' if alias else 'fresh-') + typ] = (
            f'fn work(values:[{typ}])->[{typ}]:\n  {body}\n',
            f'  values:[{typ}]=[{initial}]\n', 'work(values)',
            f'    result[0]={changed}\n    values[0]\n', typ, expected)
for name, typ, value, expected in (
    ('min-i64', 'int64', '-9223372036854775808', -9223372036854775808),
    ('max-u64', 'uint64', '18446744073709551615', 18446744073709551615),
    ('min-i8', 'int8', '-128', -128),
    ('negative-zero-f32', 'float32', '-0.0', -0.0),
    ('negative-zero-f64', 'float64', '-0.0', -0.0),
):
    cases[name] = (f'fn work(values:[{typ}])->[{typ}]:\n  copy(values)\n',
                   f'  values:[{typ}]=[{value}]\n', 'work(values)', '    result[0]\n', typ, expected)
cases['zero-input-filter-generator'] = ('''fn work(n:int)->[int]:
  for i in range(n):
    if i%2==0:
      yield i*3
''', '', 'work(10)', '    result[0]+result[1]+result[2]+result[3]+result[4]\n', 'int', 60)
cases['multi-page-generator'] = ('''fn work(n:int)->[int]:
  for i in range(n):
    yield i*2+1
''', '', 'work(17000)', '    result[0]+result[8191]+result[8192]+result[16999]\n', 'int', 66768)
cases['multi-page-capture-copy'] = ('''fn work(values:[int])->[int]:
  copy(values)
''', '  values=for i in range(17000):\n    yield i\n', 'work(values)',
    '    result[0]+result[8191]+result[8192]+result[16999]\n', 'int', 33382)
for alias in (False, True):
    cases['conditional-' + ('alias' if alias else 'copy')] = ('''fn work(values:[int],flag:Bool)->[int]:
  if flag:
    values
  else:
    copy(values)
''', '  values:[int]=[1,2,3]\n', f'work(values,{str(alias).lower()})',
        '    result[0]=7\n    values[0]\n', 'int', 7 if alias else 1)
cases['empty-copy'] = ('''fn work(values:[int])->[int]:
  copy(values)
''', '  values:[int]=[]\n', 'work(values)', '    length(result)\n', 'int', 0)
cases['empty-bool-alias'] = ('''fn work(values:[Bool])->[Bool]:
  values
''', '  values:[Bool]=[]\n', 'work(values)', '    length(result)\n', 'int', 0)
cases['record-held-capture-after-gc'] = ('''type Box:
  values:[int]
fn work(values:[int])->[int]:
  box=Box {values:values}
  for i in range(1000):
    temporary=[i,1]
    _=temporary[0]
  box.values
''', '  values:[int]=[1,2,3]\n', 'work(values)', '    result[0]=7\n    values[0]\n', 'int', 7)
cases['sorted-result'] = ('''fn work(values:[int])->[int]:
  sorted(values)
''', '  values:[int]=[9,2,5]\n', 'work(values)',
    '    result[0]*1000+result[1]*100+result[2]*10+values[0]\n', 'int', 2599)
total_cases = len(cases)
assert not set(a.case) - cases.keys(), a.case
if a.case:
    cases = {name: case for name, case in cases.items() if name in a.case}
results = []
for name, (helper, setup, call, body, typ, expected) in cases.items():
    root = a.root / name
    root.mkdir(exist_ok=True)
    cpu_source = root / 'cpu.k'
    cpu_body = '  result=' + call + '\n' + '\n'.join(line[2:] for line in body.rstrip().splitlines()) + '\n'
    cpu_source.write_text(helper + f'fn main()->{typ}:\n' + setup + cpu_body)
    cpu = root / ('cpu' + suffix)
    run([a.compiler, cpu_source, *flags, '-o', cpu])
    raw = run([cpu])
    if typ == 'Bool':
        assert raw in ('true', 'false'), (name, 'CPU Bool encoding', raw)
    value = (raw == 'true') if typ == 'Bool' else float(raw) if typ.startswith('float') else int(raw)
    assert value == expected, (name, 'CPU oracle', raw, expected)
    if typ.startswith('float'):
        assert struct.pack('<d', value) == struct.pack('<d', expected), (name, 'CPU float bits', raw)
    source = root / 'gpu.k'
    source.write_text(helper + f'fn main()->{typ} ! TaskError:\n' + setup +
                      '  try parallel:\n    gpu=devices.gpu()\n    result=try await spawn ' + call + ' on gpu\n' + body)
    bundle = root / 'device'
    arithmetic = 'hardware-with-software-fallback' if a.backend == 'metal' and typ == 'float64' else 'hardware'
    run([a.compiler, source, '--target', a.backend, '--arithmetic', arithmetic, *flags, '-o', bundle])
    assert 'DeviceArrayResultJob<' in (bundle / 'host.cpp').read_text(), name
    outputs = []
    for workers in (() if a.compile_only else ('1', '4')):
        output = run([bundle / ('program' + suffix)], dict(os.environ, KALANDRA_WORKERS=workers))
        if typ == 'Bool':
            assert output in ('true', 'false'), (name, output)
            value = output == 'true'
        else:
            bits = int(output, 16)
            width = 32 if typ == 'float32' else 64 if typ in ('float64', 'int', 'uint') else int(typ.lstrip('uint'))
            if typ.startswith('float'):
                fmt = '<f' if width == 32 else '<d'
                value = struct.unpack(fmt, bits.to_bytes(width // 8, 'little'))[0]
                assert struct.pack(fmt, value) == struct.pack(fmt, expected), (name, output)
            else:
                value = bits - (1 << width) if typ.startswith('int') and bits & (1 << (width - 1)) else bits
        assert value == expected, (name, workers, output, expected)
        outputs.append(output)
    results.append({'case': name, 'status': 'compiled' if a.compile_only else 'passed', 'outputs': outputs})
    print(name, 'CPU passed/GPU compiled' if a.compile_only else 'CPU/GPU passed', flush=True)
negatives = {
    'pending-returned-alias': ('values', 'values'),
    'pending-returned-copy': ('copy(values)', 'result'),
    'pending-returned-alias-of-alias': ('values', 'result'),
}
for name, (returned, read) in negatives.items():
    source = a.root / (name + '.k')
    source.write_text('fn work(values:[int])->[int]:\n  ' + returned +
                      '\nfn reader(values:[int])->int:\n  values[0]\n' +
                      'fn main()->int ! TaskError:\n  values:[int]=[1,2,3]\n  try parallel:\n' +
                      '    gpu=devices.gpu()\n    result=try await spawn work(values) on gpu\n' +
                      '    alias=result\n    pending=spawn reader(' + read + ')\n' +
                      '    alias[0]=7\n    try await pending\n')
    r = subprocess.run([str(a.compiler), str(source), '--target', a.backend, *flags,
                        '-o', str(a.root / name)], capture_output=True, text=True, timeout=240)
    (a.root / (name + '.log')).write_text(r.stdout + r.stderr)
    assert r.returncode != 0 and 'S077' in r.stderr, (name, r.returncode, r.stderr)
    results.append({'case': name, 'status': 'rejected', 'diagnostic': 'S077'})
    print(name, 'rejected S077', flush=True)
(a.root / 'report.json').write_text(json.dumps({'backend': a.backend, 'variant': a.variant,
                                               'complete': True, 'execution': not a.compile_only, 'totalCases': total_cases, 'negativeCases': len(negatives), 'selectedCases': a.case, 'results': results}, indent=2) + '\n')
