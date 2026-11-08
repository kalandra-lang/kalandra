"""Qualify native array identity and lifetime in mixed CPU/GPU host programs."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), default='metal')
parser.add_argument('--supervisor', action='store_true')
parser.add_argument('--compile-only', action='store_true',
                    help='Build complete GPU applications without requiring GPU execution')
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'))
args = parser.parse_args()
args.root.mkdir(parents=True, exist_ok=True)
assert not (args.supervisor and args.variant)
assert not args.compile_only or args.variant, '--compile-only requires a build variant'
fixtures = Path(__file__).resolve().parents[1] / 'tests/fixtures/mixed-host-arrays'
flags = [] if not args.variant else ['--build', {
    'debug': '-O0', 'optimized': '-O2', 'sanitized': '--sanitize',
    'thread-sanitized': '--thread-sanitize'}[args.variant]]


def run(command, expected=0, output=None, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=180, env=env)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    assert not any(token in result.stderr for token in
                   ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), result.stderr
    if output is not None:
        diagnostics = '\n'.join(line for line in result.stderr.splitlines()
            if not re.fullmatch(r'\d{4}-\d{2}-\d{2} [0-9:.]+ \S+\[\d+:\d+\] Metal API Validation Enabled', line))
        assert result.stdout == output and not diagnostics, (command, result.stdout, result.stderr, output)
    return result



def compile_command(path, destination, qualified=True):
    arithmetic = 'hardware-with-software-fallback'
    if args.supervisor:
        return [args.compiler, path, destination, arithmetic]
    return [args.compiler, '--target', args.backend, '--arithmetic', arithmetic,
            path, '-o', destination, *(flags if qualified else [])]

cases = {name: ((fixtures / (name + '.k')).read_text(), expected)
         for name, expected in [('snapshot', 1013), ('boolean', 3012),
                                ('tasks', 2538), ('functional', 239),
                                ('recovery', 27), ('empty', 210), ('loops', 20160),
                                ('yields', 62), ('yield-recovery', 42)]}
for numeric in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32',
                'u64', 'f32', 'f64'):
    cases['element-' + numeric] = (f'''fn twice(values:Tensor[{numeric},?])->Tensor[{numeric},?]:
  return values+values
fn update(mut values:[{numeric}])->[{numeric}]:
  for index in range(length(values)):
    if index==0:
      values[index]={numeric}(7)
    else:
      continue
  return values
fn main()->i64 ! TaskError:
  return try parallel:
    values=[{numeric}(3),{numeric}(1),{numeric}(2)]
    original=tensor(values)
    alias=update(values)
    values=sorted(values)
    values=concat(values,[{numeric}(4)])
    values[0]={numeric}(9)
    gpu=devices.gpu()
    resident=try await transfer(original,gpu)
    work=spawn twice(resident) on gpu
    host=try await transfer(try await work,devices.cpu())
    i64(reduceSum(host))+i64(alias[0])*100+i64(values[0])*10+length(values)
''', 806)
    if numeric in ('f32', 'u64'):
        source, _ = cases['element-' + numeric]
        remote = f'''fn fresh(values:[{numeric}])->[{numeric}]:
  result=sorted(values)
  result[0]={numeric}(7)
  return result
fn remote(values:[{numeric}])->[{numeric}] ! TaskError:
  return try parallel:
    try await spawn fresh(values) on devices.cpu()
'''
        source = source.replace('fn main()->i64 ! TaskError:', remote + 'fn main()->i64 ! TaskError:')
        source = source.replace('    alias=update(values)',
                                '    alias=update(values)\n    checkpoint=try await spawn remote(values) on devices.cpu()')
        source = source.replace('+length(values)', '+length(values)+int(checkpoint[0])')
        cases['element-' + numeric] = source, 813

cases['kernel-local-mutation'] = ('''fn edit(values:Tensor[i64,?])->Tensor[i64,?]:
  items=array(values)
  items[0]=7
  return tensor(items)
fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    resident=try await transfer(tensor([1,2,3]),gpu)
    work=spawn edit(resident) on gpu
    changed=try await transfer(try await work,devices.cpu())
    original=try await transfer(resident,devices.cpu())
    reduceSum(original)*100+reduceSum(changed)
''', 612)

results = []
for name, (source, expected) in cases.items():
    path, destination = args.root / (name + '.k'), args.root / name
    path.write_text(source)
    run(compile_command(path, destination))
    host = (destination / 'host.cpp').read_text()
    if name == 'kernel-local-mutation':
        assert 'NumericInstructionKind::DeviceControl' in host, 'local mutation must execute on device'
    else:
        assert 'HostArrayScopes arrayScopes' in host and 'HostArrayWork work' in host
        llvm = (destination / 'cpu.ll').read_text()
        assert 'kalandra_gpu_step_' in llvm and (destination / 'cpu-steps.mlir').is_file()
    if args.variant and not args.compile_only:
        for workers in (1, 2, 4):
            run([destination / 'program'], output=f'{expected:016x}\n',
                env=dict(os.environ, KALANDRA_WORKERS=str(workers), MTL_DEBUG_LAYER='1'))
    results.append({'case': name, 'expected': f'{expected:016x}',
                    'status': ('compiled' if args.compile_only else 'executed') if args.variant else 'emitted'})

# The same proof must apply before every backend's host lowering. Rejection
# must leave an existing output directory untouched.
read_only = '''fn read(values:[i64])->i64:
  return values[0]
fn main()->i64 ! TaskError:
  return try parallel:
    values=[3,1,2]
    alias=values
    task=spawn read(values) on devices.cpu()
    alias[0]=9
    try await task
'''
for name, source, diagnostic in (
    ('active-reader', read_only, 'S077'),
    ('read-only-parameter', '''fn edit(values:[i64])->i64:
  values[0]=7
  return values[0]
fn main()->i64 ! TaskError:
  return edit([1])
''', 'S077'),
):
    path, destination = args.root / (name + '.k'), args.root / name
    path.write_text(source)
    destination.mkdir(exist_ok=True)
    witness = destination / 'preserve.txt'
    witness.write_text('existing output\n')
    result = run(compile_command(path, destination, False), expected=1)
    assert diagnostic in result.stderr, result.stderr
    assert list(destination.iterdir()) == [witness] and witness.read_text() == 'existing output\n'
    results.append({'case': name, 'status': 'rejected'})
if args.variant and not args.compile_only:
    relocated = args.root / 'relocated'
    shutil.copytree(args.root / 'tasks', relocated, dirs_exist_ok=True)
    run([relocated / 'program'], output=f'{2538:016x}\n')
summary = {'backend': args.backend, 'variant': args.variant,
           'gpuExecution': bool(args.variant and not args.compile_only), 'results': results}
(args.root / 'report.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
