"""Physical Metal lifetime/resource checks for invocation-owned source arrays."""
import argparse
from pathlib import Path
import subprocess
import sys
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--metal-runtime', type=Path, required=True)
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--variant', choices=('optimized', 'sanitized', 'thread-sanitized'), required=True)
p.add_argument('--root', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo/'native/gpu-source'))
from DeviceArrayCases import cases, fault_cases, record_lifetime_cases
sources = {
    'finite': cases()['reclaim-dead-keep-alias'][0],
    'finite-map': """fn total(n:int)->int:
  sum=0
  for i in range(n):
    values=[i+1,n]
    sum=sum+values[0]
  sum
fn work(n:int)->int:
  values=generate(1,fn(i):total(n))
  values[0]
""",
    'finite-sort': """fn work(n:int)->int:
  sum=0
  for i in range(n):
    values=[n,i+1]
    sort(values)
    sum=sum+values[0]
  sum
""",
    'infinite': '''fn work(n:int)->int:
  i=0
  while true:
    values=[i,n]
    i=values[0]+1
  i
''',
    'fault': fault_cases()['bounds-before-rhs'][0],
}
sources.update({name+'-record': helper for name, helper in record_lifetime_cases().items()})
flags = {'optimized': ['-O2'], 'sanitized': ['-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'],
    'thread-sanitized': ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']}[a.variant]
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
def run(command):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout.strip()
for name, helper in sources.items():
    source = a.root/(name+'.k')
    source.write_text(helper+'''fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    try await spawn work(1000) on gpu
''')
    fixture = a.root/name
    run([a.compiler.resolve(), source, '--target', 'metal', '-O2', '-o', fixture])
    assert 'DeviceControl' in (fixture/'host.cpp').read_text(), name
    binary = a.root/(name+'-probe')
    run([a.clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off',
         '-I', repo/'native/metal', *flags,
         '-DKALANDRA_ARRAY_HOST="'+str(fixture/'host.cpp')+'"',
         repo/'native/metal-source/DeviceArrayRuntimeTests.cpp', a.metal_runtime.resolve(),
         a.runtime.resolve(), '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
    mode = [] if name.startswith('finite') else ['fault' if name.startswith('fault') else 'cancel-only']
    print(a.variant, name, run([binary, fixture, *mode]), flush=True)
