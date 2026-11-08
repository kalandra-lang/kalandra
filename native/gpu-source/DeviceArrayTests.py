"""Run equivalent CPU/device source arrays; compile-only is explicitly labelled."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
from DeviceArrayCases import cases, fault_cases
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--variant', choices=('debug', 'optimized'), default='optimized')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.root.mkdir(parents=True, exist_ok=True)
flags = ['-O0' if a.variant == 'debug' else '-O2']
results = []
suffix = '.exe' if os.name == 'nt' else ''
def run(command, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240, env=env)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    return r.stdout.strip()
for name, (helper, call, typ, expected) in cases().items():
    root = a.root/name
    root.mkdir(exist_ok=True)
    cpu = root/'cpu.k'
    cpu.write_text(helper+f'fn main()->{typ}:\n  return {call}\n')
    reference = root/('cpu'+suffix)
    run([a.compiler, cpu, *flags, '-o', reference])
    output = run([reference])
    assert float(output) == expected, (name, 'CPU', output, expected)
    gpu = root/'gpu.k'
    gpu.write_text(helper+f'''fn main()->{typ} ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn {call} on gpu
''')
    bundle = root/'device'
    arithmetic = 'hardware-with-software-fallback' if a.backend == 'metal' and typ == 'f64' else 'hardware'
    run([a.compiler, gpu, '--target', a.backend, '--arithmetic', arithmetic, *flags, '-o', bundle])
    assert 'NumericInstructionKind::DeviceControl' in (bundle/'host.cpp').read_text(), name
    assert any('gpu.func' in f.read_text() and 'scf.while' in f.read_text()
               for f in bundle.glob('*.mlir')), name
    if not a.compile_only:
        raw = run([bundle/('program'+suffix)], dict(os.environ, KALANDRA_WORKERS='1'))
        assert raw == run([bundle/('program'+suffix)], dict(os.environ, KALANDRA_WORKERS='4')), name
        bits = int(raw, 16)
        if typ.startswith('f'):
            width = 32 if typ == 'f32' else 64
            value = struct.unpack('<f' if width == 32 else '<d', bits.to_bytes(width//8, 'little'))[0]
        else:
            width = 64 if typ == 'i64' else int(''.join(filter(str.isdigit, typ)))
            value = bits - (1 << width) if typ.startswith('i') and bits & (1 << (width-1)) else bits
        assert value == expected, (name, 'GPU', raw, value, expected)
    results.append({'case': name, 'expected': expected, 'status': 'compiled' if a.compile_only else 'executed'})
    print(name, results[-1]['status'], flush=True)
# A private array helper can feed a resident tensor graph; the whole program
# need not itself have a scalar result.
root = a.root/'tensor-composition'
root.mkdir(exist_ok=True)
helper = """fn factor(n:i64)->f32:
  values=for i in range(n):
    yield f32(i)
  total:f32=0
  for value in values:
    total=total+value
  return total
fn graph(values:Tensor[f32,?],n:i64)->Tensor[f32,?]:
  return values*factor(n)
"""
cpu = root/'cpu.k'
cpu.write_text(helper+'fn main()->Tensor[f32,3]:\n  return graph(tensor([1,2,3]),5)\n')
run([a.compiler, cpu, *flags, '-o', root/('cpu'+suffix)])
assert run([root/('cpu'+suffix)]) == 'tensor([10, 20, 30], [3])'
gpu = root/'gpu.k'
gpu.write_text(helper+"""fn main()->Tensor[f32,3] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f32,3]=tensor([1,2,3])
    resident=try await transfer(values,gpu)
    result=try await spawn graph(resident,5) on gpu
    try await transfer(result,devices.cpu())
""")
bundle = root/'device'
run([a.compiler, gpu, '--target', a.backend, *flags, '-o', bundle])
assert 'DeviceControl' in (bundle/'host.cpp').read_text()
if not a.compile_only:
    for workers in ('1','4'):
        assert run([bundle/('program'+suffix)], dict(os.environ, KALANDRA_WORKERS=workers)) == '[41200000,41a00000,41f00000]'
results.append({'case':'tensor-composition', 'expected':[10,20,30],
                'status':'compiled' if a.compile_only else 'executed'})
print('tensor-composition', results[-1]['status'], flush=True)
for name, (helper, call, code) in fault_cases().items():
    root = a.root/name
    root.mkdir(exist_ok=True)
    cpu = root/'cpu.k'
    cpu.write_text(helper+'fn main()->i64:\n  return '+call+'\n')
    reference = root/('cpu'+suffix)
    run([a.compiler, cpu, *flags, '-o', reference])
    r = subprocess.run([str(reference)], capture_output=True, text=True, timeout=30)
    assert r.returncode != 0 and not r.stdout, (name, 'CPU', r)
    gpu = root/'gpu.k'
    gpu.write_text(helper+'''fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn '''+call+' on gpu\n')
    bundle = root/'device'
    run([a.compiler, gpu, '--target', a.backend, *flags, '-o', bundle])
    if not a.compile_only:
        r = subprocess.run([str(bundle/('program'+suffix))], capture_output=True, text=True, timeout=30)
        assert r.returncode != 0 and not r.stdout and f'/{code}' in r.stderr, (name, 'GPU', r)
    results.append({'case': name, 'fault': code, 'status': 'compiled' if a.compile_only else 'checked-fault'})
    print(name, results[-1]['status'], flush=True)
# A scalar integer result must not hide double arithmetic in a lifted callback.
if a.backend == 'metal':
    policy_source = a.root/'callback-float64-policy.k'
    policy_source.write_text("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return f64(i)+f64(1))
  return length(values)
fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn work(4) on gpu
""")
    rejected = subprocess.run([str(a.compiler), str(policy_source), '--target', 'metal',
                               '--arithmetic', 'hardware', *flags, '-o', str(a.root/'callback-policy')],
                              capture_output=True, text=True, timeout=180)
    assert rejected.returncode and 'Metal has no hardware float64 arithmetic' in rejected.stderr, rejected
    run([a.compiler, policy_source, '--target', 'metal', '--arithmetic',
         'hardware-with-software-fallback', *flags, '-o', a.root/'callback-policy'])
    if not a.compile_only:
        assert run([a.root/'callback-policy'/('program'+suffix)]) == '0000000000000004'
(a.root/'report.json').write_text(json.dumps({'backend': a.backend, 'variant': a.variant,
    'gpuExecution': not a.compile_only, 'results': results}, indent=2)+'\n')
