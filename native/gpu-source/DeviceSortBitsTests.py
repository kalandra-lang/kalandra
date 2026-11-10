"""Physical source-sort bit tests: raw inputs, independent ordering, no FP conversion."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--driver', type=Path)
p.add_argument('--clang', type=Path)
p.add_argument('--metal-runtime', type=Path)
p.add_argument('--runtime', type=Path)
p.add_argument('--variant', choices=('optimized', 'sanitized', 'thread-sanitized'), default='optimized')
a = p.parse_args()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[2]
def run(command):
    r = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=240)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(x in r.stderr for x in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout
if a.backend == 'metal':
    assert a.clang and a.metal_runtime and a.runtime
    flags = {'optimized': ['-O2'],
             'sanitized': ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'],
             'thread-sanitized': ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']}[a.variant]
    driver = a.root/'raw-sort-driver'
    run([a.clang, '-std=c++23', '-Wall', '-Wextra', '-Werror', *flags,
         '-DKALANDRA_SORT_METAL', repo/'native/gpu-source/DeviceSortBitsTests.cpp',
         a.metal_runtime.resolve(), a.runtime.resolve(), '-framework', 'Foundation',
         '-framework', 'Metal', '-o', driver])
else:
    assert a.driver
    driver = a.driver.resolve()
results = []
for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'):
    for operation in ('sort', 'sorted'):
        root = a.root/(typ+'-'+operation)
        root.mkdir(exist_ok=True)
        source = root/'source.k'
        source.write_text(f'''fn work(a:{typ},b:{typ},c:{typ},d:{typ},index:i64)->{typ}:
  values=[a,b,c,d]
  ordered={operation}(values)
  return ordered[index]
fn main()->{typ} ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn work({typ}(0),{typ}(1),{typ}(2),{typ}(3),1) on gpu
''')
        bundle = root/'device'
        # Sorting float64 only compares its integer bits: it needs no fallback,
        # including Metal. Require hardware-only admission for this entire test.
        run([a.compiler.resolve(), '--test-entry', 'main', source, '--target', a.backend, '--arithmetic', 'hardware',
             '-O2', '-o', bundle])
        constants = {}
        control = None
        for file in bundle.glob('*.mlir'):
            if '.' in file.stem:
                continue
            text = file.read_text()
            signature = re.search(r'gpu.func @kalandra_numeric_v1_([a-z0-9]+)[^(]*\([^\n]*memref<(\d+)xi32>', text)
            if not signature:
                continue
            words = int(signature[2])
            if 'scf.while' in text:
                assert control is None, 'multiple control kernels'
                control = file
                frame = (words-2)//2
            elif words in (3, 4):
                constants[signature[1]] = file
        # Kernel resource labels retain their storage spellings; source and
        # raw-driver arguments use the canonical language primitive names.
        storage = {'i': 'int', 'u': 'uint', 'f': 'float'}[typ[0]] + typ[1:]
        assert control and storage in constants and 'int64' in constants, (typ, constants, control)
        extension = {'metal': '.metallib', 'vulkan': '.spv', 'cuda': '.ptx'}[a.backend]
        files = [constants[storage].with_suffix(extension), constants['int64'].with_suffix(extension),
                 control.with_suffix(extension)]
        output = run([driver, typ, frame, *files])
        assert 'PASS: 320 raw-bit source sorts' in output, output
        print(typ, operation, output.strip(), flush=True)
        results.append({'type': typ, 'operation': operation, 'gpuExecution': True,
                        'sourceSHA256': hashlib.sha256(source.read_bytes()).hexdigest(),
                        'shaderSHA256': {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
                        'stdout': output})
(a.root/'report.json').write_text(json.dumps({'backend': a.backend, 'variant': a.variant,
    'rawBitExecutions': 320*len(results), 'results': results}, indent=2)+'\n')
