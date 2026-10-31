"""Cross-compile private-pair/public-scalar entries with the Win64 ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--driver', type=Path, required=True)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
ir = a.output / 'windows-ordinary.ll'
commands = []
def run(command):
    commands.append(list(map(str, command)))
    result = subprocess.run(commands[-1], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, (commands[-1], result.returncode, result.stdout[-1000:], result.stderr[-3000:])
    return result.stdout
run([a.driver, ir])
text = ir.read_text()
assert 'define internal { i64, i64 } @win0_ordinary_pair_v1(' in text
assert 'define dso_local zeroext i1 @win0_ordinary_direct_v1(i1' in text
assert 'define dso_local double @win10_ordinary_direct_v1(double' in text
assert '@_mlir_ciface_kalandra_float64_entry_ordinary_direct_v1()' in text
records = []
for architecture, target, machine in [('x86_64', 'x86_64-pc-windows-msvc', b'\x64\x86'),
                                      ('arm64', 'aarch64-pc-windows-msvc', b'\x64\xaa')]:
    modes = [('debug', ['-O0']), ('optimized', ['-O2'])]
    if architecture == 'x86_64':
        modes.append(('sanitized', ['-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']))
    for name, flags in modes:
        binary = a.output / (architecture + '-' + name + '.obj')
        run([a.clang, '-target', target, '-Wno-override-module',
             '-ffp-contract=off', *flags, '-c', ir, '-o', binary])
        # COFF machine identity: inspected only; hosted suites execute natively.
        data = binary.read_bytes()
        assert data[:2] == machine, 'COFF architecture mismatch'
        records.append(dict(architecture=architecture, variant=name,
                            sha256=hashlib.sha256(data).hexdigest(), bytes=len(data)))
(a.output / 'qualification.json').write_text(json.dumps(dict(
    scope='Code-generation qualification only. Win64 public scalar entries and private '
          'aggregate helpers. Hosted Windows source suites verify runtime ABI parity.',
    driver_sha256=hashlib.sha256(a.driver.read_bytes()).hexdigest(),
    ir_sha256=hashlib.sha256(ir.read_bytes()).hexdigest(), records=records, commands=commands), indent=2) + '\n')
print('Windows x86-64/ARM64 scalar entries and aggregate helpers compiled')
