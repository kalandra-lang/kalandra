"""Native array main results from GPU-targeted programs use the array task ABI."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), default='optimized')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
flags = {'debug': ['-O0'], 'optimized': ['-O2'], 'sanitized': ['--sanitize'],
         'thread-sanitized': ['--thread-sanitize']}[a.variant]
if a.backend != 'metal' and a.variant not in ('debug', 'optimized'):
    p.error('GPU runtime sanitizer variants currently require Metal')
suffix = '.exe' if os.name == 'nt' else ''

def run(command, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240, env=env)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout.strip()

cases = []
for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64', 'bool'):
    if typ == 'bool':
        source_values, expected = ['true', 'false', 'true'], ['true', 'false', 'true']
    elif typ.startswith('f'):
        source_values, expected = ['-0.0', '1.5', '3.0'], [-0.0, 1.5, 3.0]
    else:
        width = int(typ.lstrip('u64'))
        signed = typ.startswith('i')
        expected = [-(1 << (width - 1)), 0, (1 << (width - 1)) - 1] if signed else [0, 1, (1 << width) - 1]
        source_values = list(map(str, expected))
    for alias in (False, True):
        cases.append((typ, alias, source_values, expected, False))
for typ in ('i64', 'bool'):
    cases.append((typ, False, [], [], True))
results = []
for typ, alias, source_values, expected, empty in cases:
    name = ('empty-' if empty else 'alias-' if alias else 'fresh-') + typ
    root = a.root / name
    root.mkdir(exist_ok=True)
    helper = f'fn work(values:[{typ}])->[{typ}]:\n  ' + ('values' if alias else 'copy(values)') + '\n'
    setup = f'  values:[{typ}]=[' + ','.join(source_values) + ']\n'
    cpu_source = root / 'cpu.k'
    cpu_source.write_text(helper + f'fn main()->[{typ}]:\n' + setup + '  work(values)\n')
    cpu = root / ('cpu' + suffix)
    run([a.compiler, cpu_source, *flags, '-o', cpu])
    raw = run([cpu, '--all'])
    assert raw.startswith('[') and raw.endswith(']'), (name, 'CPU collection format', raw)
    values = [] if raw == '[]' else [v.strip() for v in raw[1:-1].split(',')]
    if typ == 'bool':
        assert values == expected, (name, raw, expected)
        encoded = expected
    elif typ.startswith('f'):
        fmt, width = ('<f', 32) if typ == 'f32' else ('<d', 64)
        assert [struct.pack(fmt, float(v)) for v in values] == [struct.pack(fmt, v) for v in expected], (name, raw)
        encoded = [format(int.from_bytes(struct.pack(fmt, v), 'little'), f'0{width // 4}x') for v in expected]
    else:
        width = int(typ.lstrip('u64'))
        assert list(map(int, values)) == expected, (name, raw, expected)
        encoded = [format(v & ((1 << width) - 1), f'0{width // 4}x') for v in expected]
    expected_gpu = '[' + ','.join(encoded) + ']'
    source = root / 'gpu.k'
    source.write_text(helper + f'fn main()->[{typ}] ! TaskError:\n' + setup +
                      '  try parallel:\n    gpu=devices.gpu()\n    try await spawn work(values) on gpu\n')
    bundle = root / 'device'
    run([a.compiler, source, '--target', a.backend, '--arithmetic', 'hardware', *flags, '-o', bundle])
    host = (bundle / 'host.cpp').read_text()
    assert 'const ValueLayout &mainArrayLayout=hostArrayLayout<' in host, name
    assert 'mainArrayLayout,context,&value)' in host, name
    outputs = []
    for workers in (() if a.compile_only else ('1', '4')):
        output = run([bundle / ('program' + suffix)], dict(os.environ, KALANDRA_WORKERS=workers))
        assert output == expected_gpu, (name, workers, output, expected_gpu)
        outputs.append(output)
    results.append({'case': name, 'status': 'compiled' if a.compile_only else 'passed', 'outputs': outputs})
    print(name, results[-1]['status'], flush=True)
(a.root / 'report.json').write_text(json.dumps({'complete': True, 'execution': not a.compile_only,
    'backend': a.backend, 'variant': a.variant, 'results': results}, indent=2) + '\n')
