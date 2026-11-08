"""Run physical Vulkan/CUDA lifetime checks against compiler-generated control.

The finite program carries a plain record through a loop. The runtime driver
independently checks sums, concurrent inputs, cancellation/fault precedence and
resource retirement. This is an explicit hardware test, never a compile-only
substitute.
"""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--driver', type=Path, required=True)
p.add_argument('--backend', choices=('vulkan', 'cuda'), required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--arrays', action='store_true')
p.add_argument('--sort', action='store_true')
p.add_argument('--records', action='store_true', help='record-held array roots; requires --arrays')
a = p.parse_args()
a.root.mkdir(parents=True, exist_ok=True)
sources = {
    'finite': '''type Step:
  value:i64
  enabled:bool
fn work(n:i64)->i64:
  i=0
  total=0
  while i<n:
    i=i+1
    step=Step {value:i,enabled:true}
    if step.enabled:
      total=total+step.value
  return total
''',
    'infinite': '''fn work(n:i64)->i64:
  i=0
  while true:
    i=i+1
  return i
''',
    'fault': '''fn work(n:i64)->i64:
  i=0
  total=0
  while i<n:
    total=total+1/(n-n)
    i=i+1
  return total
''',
}
if a.arrays:
    from DeviceArrayCases import cases, fault_cases
    sources['finite'] = cases()['reclaim-dead-keep-alias'][0]
    sources['infinite'] = 'fn work(n:i64)->i64:\n  i=0\n  while true:\n    values=[i,n]\n    i=values[0]+1\n'
    sources['fault'] = fault_cases()['bounds-before-rhs'][0]
if a.records:
    assert a.arrays and not a.sort, '--records requires --arrays without --sort'
    from DeviceArrayCases import record_lifetime_cases
    sources = record_lifetime_cases()
if a.sort:
    assert a.arrays, '--sort requires --arrays'
    sources['finite'] = """fn work(n:i64)->i64:
  values=generate(n,fn(i):return n-i)
  sort(values)
  total=0
  for value in values:
    total=total+value
  return total
"""
    sources['infinite'] = """fn work(n:i64)->i64:
  i=0
  while true:
    values=[n,i]
    sort(values)
    i=values[0]+1
  return i
"""
shaders = []
for name, helper in sources.items():
    source = a.root/(name+'.k')
    source.write_text(helper + '''fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn work(1000) on gpu
''')
    bundle = a.root/name
    subprocess.run([str(a.compiler.resolve()), str(source), '--target', a.backend,
                    '-O2', '-o', str(bundle)], check=True, timeout=180)
    assert ('DeviceControl' if a.arrays else 'ScalarControl') in (bundle/'host.cpp').read_text(), name
    control = [f for f in bundle.glob('*.mlir')
               if 'gpu.func' in f.read_text() and 'scf.while' in f.read_text()]
    # The initial artifact is the source form; converted/lowered artifacts can
    # also contain scf.while. Select the emitted control stage by exact suffix.
    extension = '.ptx' if a.backend == 'cuda' else '.spv'
    stages = [f.with_suffix(extension) for f in control
              if '-stage' in f.stem and f.with_suffix(extension).is_file()
              and not any(x in f.stem for x in ('.lowered', '.converted', '.kernel'))]
    assert len(stages) == 1, (name, stages)
    shaders.append(stages[0])
result = subprocess.run([str(a.driver.resolve()), *map(str, shaders), *(['sorting' if a.sort else 'arrays'] if a.arrays else [])],
                        capture_output=True, text=True, timeout=180)
print(result.stdout, end='')
print(result.stderr, end='')
assert result.returncode == 0, result.returncode
assert 'PASS:' in result.stdout
(a.root/'report.json').write_text(json.dumps({
    'backend': a.backend, 'gpuExecution': True,
    'sorting': a.sort,
    'finiteStorage': 'invocation-local numeric arrays' if a.arrays else 'Step(value:i64, enabled:bool)',
    'driverChecks': ['prepared-reuse', 'distinct-concurrent-captures',
                     'infinite-cancellation', 'deadlines', 'fault-precedence',
                     'zero-retained-resources'],
    'shaders': list(map(str, shaders)), 'stdout': result.stdout,
}, indent=2)+'\n')
