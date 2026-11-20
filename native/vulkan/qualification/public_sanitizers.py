"""Require bit-exact results and clean ASan, UBSan and LeakSanitizer exits."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
source_group = parser.add_mutually_exclusive_group(required=True)
source_group.add_argument('--compiler', type=Path)
source_group.add_argument('--artifacts', type=Path, help='execute previously compiled sanitizer bundles')
parser.add_argument('--nvidia-compute', action='store_true',
                    help='use the headless NVIDIA driver workaround for issue #298')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--compile-only', action='store_true')
args = parser.parse_args()
if args.artifacts and args.compile_only:
    parser.error('--artifacts requires physical execution')
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
matrix = args.output/'matrix.k'
if args.compiler:
    matrix.write_text('''fn work(a:Tensor[f32,?,?],b:Tensor[f32,?,?])->Tensor[f32,?,?]:
  return matmul(a,b)
fn main()->Tensor[f32,?,?] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    a:Tensor[f32,2,3]=tensor([[1,2,3],[4,5,6]])
    b:Tensor[f32,3,2]=tensor([[1,0],[0,1],[1,1]])
    left=try await transfer(a,gpu)
    right=try await transfer(b,gpu)
    result=try await spawn work(left,right) on gpu
    try await transfer(result,devices.cpu())
''')
device_settings = {
    name: {'VK_DRIVER_FILES': f'/usr/share/vulkan/icd.d/{name}_icd.x86_64.json'}
    for name in ('nvidia', 'intel')
}
if args.nvidia_compute and not args.compile_only:
    manifest = args.output/'nvidia-compute.json'
    manifest.write_bytes(subprocess.check_output([
        sys.executable, repo/'native/vulkan/NvidiaComputeIcd.py',
        device_settings['nvidia']['VK_DRIVER_FILES']]))
    device_settings['nvidia'] = {'VK_DRIVER_FILES': str(manifest), '__GL_DisableDBus': '1'}
records = []
failures = []
for name, source, values in [('placed', repo/'native/metal-source/placed.k', [5, 12, 21]),
                             ('mixed', repo/'native/metal-source/cpu-spawn.k', [52, 54, 64, 66]),
                             ('matrix', matrix, [4, 5, 10, 11])]:
    output = (args.artifacts or args.output).resolve()/name
    if args.compiler:
        built = subprocess.run(list(map(str, [args.compiler.resolve(), '--test-entry', 'main', '--target', 'vulkan',
                                               source, '--sanitize', '--matmul-policy', 'optimized', '-o', output])),
                               capture_output=True, text=True, timeout=180)
        (args.output/(name+'-build.log')).write_text(built.stdout+built.stderr)
        built.check_returncode()
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
    for device, device_env in device_settings.items():
        for workers in (1, 2, 4):
            env = os.environ | device_env | {
                'KALANDRA_WORKERS': str(workers),
                'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                'LSAN_OPTIONS': 'detect_leaks=1:exitcode=23',
                'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
            }
            execution = subprocess.run([output/'program'], env=env, capture_output=True,
                                       text=True, timeout=90)
            record = {'case': name, 'device': device, 'workers': workers,
                      'nvidia_compute': device == 'nvidia' and args.nvidia_compute,
                      'status': execution.returncode, 'stdout': execution.stdout, 'stderr': execution.stderr}
            records.append(record)
            (args.output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
            if execution.returncode or execution.stderr or execution.stdout != expected:
                failures.append(f'{name}/{device}/{workers}')
                print('FAIL:', failures[-1], 'exit', execution.returncode, flush=True)
                continue
            print('PASS:', name, device, workers, 'exact output and clean sanitizer exit', flush=True)
if failures:
    raise SystemExit(f'{len(failures)} failed ({", ".join(failures)}); see {args.output/"results.json"}')
print('PASS: instrumented host/runtime/CPU export selection;', len(records), 'physical executions.', flush=True)
