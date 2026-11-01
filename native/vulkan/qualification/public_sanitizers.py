"""Qualify host/CPU ASan/UBSan selection, retaining unsuppressed loader leaks."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--compile-only', action='store_true')
args = parser.parse_args()
args.compiler = args.compiler.resolve()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
matrix = args.output/'matrix.k'
matrix.write_text('''fn work(a:Tensor[float32,?,?],b:Tensor[float32,?,?])->Tensor[float32,?,?]:
  matmul(a,b)
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    a:Tensor[float32,2,3]=tensor([[1,2,3],[4,5,6]])
    b:Tensor[float32,3,2]=tensor([[1,0],[0,1],[1,1]])
    left=try await transfer(a,gpu)
    right=try await transfer(b,gpu)
    result=try await spawn work(left,right) on gpu
    try await transfer(result,devices.cpu())
''')
records = []
for name, source, values in [('placed', repo/'native/metal-source/placed.k', [5, 12, 21]),
                             ('mixed', repo/'native/metal-source/cpu-spawn.k', [52, 54, 64, 66]),
                             ('matrix', matrix, [4, 5, 10, 11])]:
    output = args.output/name
    built = subprocess.run(list(map(str, [args.compiler, '--target', 'vulkan', source,
                                           '--sanitize', '--matmul-policy', 'optimized', '-o', output])), capture_output=True, text=True, timeout=180)
    (args.output/(name+'-build.log')).write_text(built.stdout+built.stderr)
    assert not built.returncode, built.stderr[-4000:]
    assert 'host_instrumentation=address,undefined' in (output/'manifest.txt').read_text()
    if name == 'matrix':
        assert 'matmul_policy=optimized' in (output/'manifest.txt').read_text()
        assert (output/'kernel0.spv.fixed-tree.spv').is_file()
    symbols = subprocess.check_output(['nm', str(output/'program')], text=True)
    assert '__asan_init' in symbols and '__ubsan_handle_' in symbols
    if name == 'mixed':
        assert 'sanitize_address' in (output/'cpu.ll').read_text()
    if args.compile_only:
        continue
    expected = '['+','.join(f'{struct.unpack("<I", struct.pack("<f", value))[0]:08x}' for value in values)+']\n'
    for device, icd in [('nvidia', 'nvidia_icd.x86_64.json'), ('intel', 'intel_icd.x86_64.json')]:
        for workers in (1, 2, 4):
            execution = subprocess.run([output/'program'], capture_output=True, text=True, timeout=90,
                env=os.environ | {'VK_DRIVER_FILES': '/usr/share/vulkan/icd.d/'+icd,
                                  'KALANDRA_WORKERS': str(workers),
                                  'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
            record = {'case': name, 'device': device, 'workers': workers,
                      'status': execution.returncode, 'stdout': execution.stdout, 'stderr': execution.stderr}
            records.append(record)
            (args.output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
            assert execution.stdout == expected, record
            assert 'runtime error:' not in execution.stderr and 'ERROR: AddressSanitizer:' not in execution.stderr, record
            leak = 'ERROR: LeakSanitizer: detected memory leaks' in execution.stderr
            assert ((execution.returncode == 1 and leak and 'SUMMARY: AddressSanitizer:' in execution.stderr)
                    or (execution.returncode == 0 and not execution.stderr)), record
            print('PASS:', name, device, workers, 'numeric/ownership execution;',
                  'UNSUPPRESSED LSAN EXIT FAILURE RETAINED' if leak else 'clean sanitizer exit', flush=True)
print('PASS: instrumented host/runtime/CPU export selection;', len(records),
      'physical executions. Exit-time leaks, if present, remain open findings.', flush=True)
