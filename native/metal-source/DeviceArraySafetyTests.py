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
p.add_argument('--entries-only', action='store_true')
p.add_argument('--results-only', action='store_true')
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(repo/'native/gpu-source'))
from DeviceArrayCases import cases, fault_cases, record_lifetime_cases
sources = {
    'finite': cases()['reclaim-dead-keep-alias'][0],
    'finite-map': """fn total(n:i64)->i64:
  sum=0
  for i in range(n):
    values=[i+1,n]
    sum=sum+values[0]
  return sum
fn work(n:i64)->i64:
  values=generate(1,fn(i):return total(n))
  return values[0]
""",
    'finite-sort': """fn work(n:i64)->i64:
  sum=0
  for i in range(n):
    values=[n,i+1]
    sort(values)
    sum=sum+values[0]
  return sum
""",
    'infinite': '''fn work(n:i64)->i64:
  i=0
  while true:
    values=[i,n]
    i=values[0]+1
  return i
''',
    'fault': fault_cases()['bounds-before-rhs'][0],
}
sources.update({name+'-record': helper for name, helper in record_lifetime_cases().items()})
sources.update({
    'finite-entry': '''fn work(values:[i64],n:i64)->i64:
  total=0
  for i in range(n):
    local=[i+values[0],n]
    total=total+local[0]
  return total
''',
    'infinite-entry': '''fn work(values:[i64],n:i64)->i64:
  i=0
  while true:
    local=[i,values[0]]
    i=local[0]+1
  return i
''',
    'fault-entry': '''fn work(values:[i64],n:i64)->i64:
  return values[2]+1/(n-n)
'''
})
if a.entries_only:
    sources={name:helper for name,helper in sources.items() if name.endswith('-entry')}
if a.results_only:
    sources={
      'finite-result': """fn work(values:[i64],n:i64)->[i64]:
  total=0
  for i in range(n):
    local=[i+1,values[0]]
    total=total+local[0]
  return [total,values[0]]
""",
      'infinite-result': """fn work(values:[i64],n:i64)->[i64]:
  i=0
  while true:
    local=[i,values[0]]
    i=local[0]+1
  return [i]
""",
      'fault-result': """fn work(values:[i64],n:i64)->[i64]:
  return [values[2],1/(n-n)]
"""
    }


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
    if a.results_only:
        source.write_text(helper+'''fn main()->i64 ! TaskError:
  values:[i64]=[1]
  return try parallel:
    gpu=devices.gpu()
    result=try await spawn work(values,1000) on gpu
    result[0]
''')
    else:
        source.write_text(helper+('''fn main()->i64 ! TaskError:
  values:[i64]=[1]
  return try parallel:
    gpu=devices.gpu()
    try await spawn work(values,1000) on gpu
''' if name.endswith('-entry') else '''fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn work(1000) on gpu
'''))
    fixture = a.root/name
    mode_flag={'optimized':'-O2','sanitized':'--sanitize',
               'thread-sanitized':'--thread-sanitize'}[a.variant]
    run([a.compiler.resolve(), source, '--target', 'metal', mode_flag, '-o', fixture])
    assert 'DeviceControl' in (fixture/'host.cpp').read_text(), name
    binary = a.root/(name+'-probe')
    run([a.clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', '-ffp-contract=off',
         '-I', repo/'native/metal', '-mmacosx-version-min=26.0', *flags,
         '-DKALANDRA_ARRAY_HOST="'+str(fixture/'host.cpp')+'"',
         repo/'native/metal-source/DeviceArrayRuntimeTests.cpp',
         *([fixture/'cpu.ll'] if (fixture/'cpu.ll').exists() else []), a.metal_runtime.resolve(),
         a.runtime.resolve(), '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
    mode = [] if name.startswith('finite') else ['fault' if name.startswith('fault') else 'cancel-only']
    print(a.variant, name, run([binary, fixture, *mode]), flush=True)
