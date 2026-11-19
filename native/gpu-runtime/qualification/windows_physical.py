"""Prepare and replay Windows x64 GPU qualification using installed compilers."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import struct
import subprocess
import tempfile
import time
from datetime import datetime, timezone

REPO = Path(__file__).resolve().parents[3]
BACKENDS = ('cuda', 'vulkan')
TYPES = ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'f32', 'f64')
ARITHMETIC = ('hardware', 'software', 'hardware-with-software-fallback')
WORKERS = (1, 2, 4)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_report(path, data):
    staging = path.with_suffix('.tmp')
    staging.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    staging.replace(path)


def numeric_output(type_name, values, *, tensor):
    width = int(type_name[1:])
    fmt = {'f32': '<f', 'f64': '<d'}.get(type_name)
    words = [
        int.from_bytes(struct.pack(fmt, value), 'little') if fmt else value % (1 << width)
        for value in values
    ]
    text = ','.join(f'{word:0{width // 4}x}' for word in words)
    return (f'[{text}]\n' if tensor else text + '\n').encode('ascii')


def tensor_map(type_name, rank, expression):
    values = 7 if rank == 0 else [1, 2, 3]
    for _ in range(max(rank - 1, 0)):
        values = [values]
    dims = ',?' * rank
    shape = ',' + ','.join(['1'] * (rank - 1) + ['3']) if rank else ''
    return f"""fn work(x:Tensor[{type_name}{dims}])->Tensor[{type_name}{dims}]:
  return {expression}
fn main()->Tensor[{type_name}{dims}] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    x:Tensor[{type_name}{shape}]=tensor({values})
    resident=try await transfer(x,gpu)
    result=try await spawn work(resident) on gpu
    try await transfer(result,devices.cpu())
"""


def reduction(type_name):
    values = [0] * 256 + [5]
    return f"""fn work(x:Tensor[{type_name},?])->{type_name}:
  return reduceSum(x)
fn main()->{type_name} ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    x:Tensor[{type_name},257]=tensor({values})
    resident=try await transfer(x,gpu)
    try await spawn work(resident) on gpu
"""


def matrix(type_name, left, right):
    m, k, n = len(left), len(right), len(right[0])
    reference = [sum(lhs[i] * right[i][column] for i in range(k))
                 for lhs in left for column in range(n)]
    source = f"""fn work(a:Tensor[{type_name},?,?],b:Tensor[{type_name},?,?])->Tensor[{type_name},?,?]:
  return matmul(a,b)
fn main()->Tensor[{type_name},?,?] ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    a:Tensor[{type_name},{m},{k}]=tensor({left})
    b:Tensor[{type_name},{k},{n}]=tensor({right})
    x=try await transfer(a,gpu)
    y=try await transfer(b,gpu)
    result=try await spawn work(x,y) on gpu
    try await transfer(result,devices.cpu())
"""
    return source, numeric_output(type_name, reference, tensor=True)


def build(command, log):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    log.write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f'Build failed ({result.returncode}). See {log}')


def prepare_backend(root, backend, compiler, cases):
    root.mkdir(parents=True)
    labels = {'cuda': 'CUDA', 'vulkan': 'Vulkan'}

    def application(name, source, *, expected=None, flags=(), test_entry=True):
        path = root/(name + '.k')
        path.write_text(source, encoding='utf-8')
        destination = root/name
        entry = ('--test-entry', 'main') if test_entry else ()
        build([compiler, *entry, '--target', backend, '-O2', *flags, path, '-o', destination],
              root/(name + '.build.log'))
        if not (destination/'program.exe').is_file():
            raise RuntimeError(f'Missing {destination}/program.exe')
        if expected is not None:
            cases.append({'path': f'{backend}/{name}/program.exe', 'stdout': expected.hex()})
        return destination

    for type_name in TYPES:
        source, expected = matrix(type_name, [[1, 2, 3], [0, 2, 1]], [[2, 1], [1, 3], [0, 2]])
        for policy in ARITHMETIC:
            flags = ('--arithmetic', policy)
            for rank in (0, 1, 8):
                values = [9] if rank == 0 else [3, 4, 5]
                application(f'{type_name}-map-rank{rank}-{policy}',
                            tensor_map(type_name, rank, 'x+2'),
                            expected=numeric_output(type_name, values, tensor=True), flags=flags)
            application(f'{type_name}-reduce-{policy}', reduction(type_name),
                        expected=numeric_output(type_name, [5], tensor=False), flags=flags)
            application(f'{type_name}-matrix-{policy}', source, expected=expected,
                        flags=(*flags, '--matmul-policy', 'fixed-tree'))

    for m, k, n in ((128, 32, 128), (129, 17, 128)):
        source, expected = matrix('f32', [[1]*k for _ in range(m)], [[1]*n for _ in range(k)])
        for policy in ('fixed-tree', 'optimized'):
            destination = application(f'f32-{m}x{k}x{n}-{policy}', source, expected=expected,
                                      flags=('--matmul-policy', policy))
            if backend == 'cuda' and (destination/'kernel0.ptx.fixed-tree.ptx').exists():
                raise RuntimeError(f'Unexpected CUDA catalog for {destination.name}')

    if backend == 'cuda':
        source, expected = matrix('f32', [[1]*32 for _ in range(128)], [[1]*256 for _ in range(32)])
        destination = application('f32-128x32x256-optimized', source, expected=expected,
                                  flags=('--matmul-policy', 'optimized'))
        if not (destination/'kernel0.ptx.fixed-tree.ptx').is_file():
            raise RuntimeError(f'Missing CUDA catalog for {destination.name}')
        cases[-1].update(cudaDiagnostics=True,
                         stderr='CUDA matmul route=tiled tile=64 shape=128,32,256\n')

    placed = (REPO/'native/metal-source/placed.k').read_text(encoding='utf-8')
    application('placed', placed, expected=numeric_output('f32', [5, 12, 21], tensor=True))
    text_sources = REPO/'native/gpu-source'
    for name, expected in (
        ('workgroup-text', b'11411 0 3211 3200 4100\n'),
        ('device-text-result', 'س\0/é猫🧪\n'.encode('utf-8')),
    ):
        application(name, (text_sources/(name + '.k')).read_text(encoding='utf-8'),
                    expected=expected, test_entry=False)
    cases.append({'path': f'{backend}/device-text-result/program.exe',
                  'arguments': ['negative-index'], 'exit': 1, 'stdout': '',
                  'stderr': f'{labels[backend]} task failed: 1/5\n'})
    cases.append({'path': f'{backend}/placed/program.exe',
                  'missing': 'kernel0.ptx' if backend == 'cuda' else 'kernel0.spv', 'exit': 1,
                  'stdout': '', 'stderrPrefix': f'{labels[backend]} task failed:'})

    # Use the same argument layout and map instructions as the runtime probes
    # used in the Linux qualification, produced by the installed Windows CLI.
    if backend == 'cuda':
        kernels = application('runtime-map', tensor_map('f32', 1, 'x+3'))
        arguments = [kernels/'kernel0.ptx']
        libraries = ('cuda-language', 'cuda-program', 'cuda')
        marker = 'PASS: source-generated CUDA graph'
    else:
        numeric_flags = ('--arithmetic', 'software')
        addition = application('float32', tensor_map('f32', 1, 'x+3'), flags=numeric_flags)
        application('uint32', tensor_map('u32', 1, 'x*2'), flags=numeric_flags)
        # The existing probe expects addition as kernel 1 of a two-stage test.
        # Compile it independently since the public compiler can fuse both stages.
        shutil.copyfile(addition/'kernel0.spv', addition/'kernel1.spv')
        arguments = [root, '0', '--no-validation']
        libraries = ('vulkan-async', 'vulkan-program', 'vulkan')
        marker = 'PASS: managed resident programs'
    runtime = compiler.parent.parent/'lib/kalandra/native'
    clang = compiler.parent.parent/'libexec/kalandra/clang++.exe'
    resources = runtime.parent/'toolchain/clang'
    test = root/'async-program-tests.exe'
    build([
        clang, '-std=c++23', '-O2', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
        '-D_CRT_SECURE_NO_WARNINGS', '-fms-runtime-lib=static', '--rtlib=compiler-rt',
        f'-resource-dir={resources}', '-I', runtime/f'headers/{backend}',
        '-I', runtime/'headers/include', REPO/f'native/{backend}/tests/AsyncProgramTests.cpp',
        *(runtime/f'kalandra-{library}.lib' for library in (*libraries, 'native-runtime')),
        '-o', test,
    ], root/'async-program-tests.build.log')
    cases.append({'path': f'{backend}/{test.name}', 'stdoutPrefix': marker,
                  'arguments': [str(arg.relative_to(root.parent)) if isinstance(arg, Path) else arg
                                for arg in arguments],
                  'bundleArguments': [i for i, arg in enumerate(arguments) if isinstance(arg, Path)],
                  'workers': [1]})


def files(root):
    return {p.relative_to(root).as_posix(): digest(p) for p in sorted(root.rglob('*'))
            if p.is_file()}


def prepare(args):
    check_windows()
    args.bundle.mkdir(parents=True, exist_ok=False)
    cases = []
    compilers = {}
    for backend, package in zip(BACKENDS, (args.cuda, args.vulkan), strict=True):
        metadata = json.loads((package/f'share/kalandra-native-{backend}/toolchain.json').read_text())
        if metadata['platform'] != 'windows-x86_64':
            raise ValueError(f'{package} is not a Windows x64 {backend} package')
        compiler = package/'bin/kalandra.exe'
        compilers[backend] = {'compilerSHA256': digest(compiler),
                              'manifestSHA256': digest(package/'SHA256SUMS')}
        prepare_backend(args.bundle/backend, backend, compiler, cases)
    write_report(args.bundle/'frozen.json', {
        'format': 1, 'revision': args.revision, 'compilers': compilers, 'cases': cases,
        'files': inventory(args.bundle),
    })
    print(f'Prepared {len(cases)} correctness cases. No GPU executions.', flush=True)


def check_windows():
    if os.name != 'nt' or platform.machine().lower() not in ('amd64', 'x86_64'):
        raise RuntimeError('This qualification requires Windows x64')


def hardware():
    check_windows()
    cards = subprocess.check_output([
        'nvidia-smi', '--query-gpu=name,pci.bus_id,driver_version,memory.total',
        '--format=csv,noheader,nounits',
    ], text=True, timeout=20).strip().splitlines()
    if len(cards) != 1 or cards[0].split(',', 1)[0].strip() != 'NVIDIA GeForce RTX 5090':
        raise RuntimeError(f'Expected exactly one RTX 5090 in the qualification guest, got {cards}')
    return {'guestOS': platform.platform(), 'machine': platform.machine(),
            'logicalCPUs': os.cpu_count(), 'gpu': cards[0]}


def inventory(bundle):
    return {f'{backend}/{relative}': checksum for backend in BACKENDS
            for relative, checksum in files(bundle/backend).items()}


def verify(bundle, manifest):
    if inventory(bundle) != manifest['files']:
        raise RuntimeError('Qualification files differ from the frozen build')


def run_case(bundle, case, workers, scratch):
    binary = bundle/case['path']
    if 'missing' in case:
        copied = scratch/binary.parent.parent.name
        shutil.copytree(binary.parent, copied, dirs_exist_ok=True)
        (copied/case['missing']).unlink()
        binary = copied/binary.name
    # Disallow inherited overrides for selecting a different Vulkan device/ICD.
    env = {name: value for name, value in os.environ.items()
           if not name.startswith(('VK_', 'CUDA_VISIBLE_DEVICES', 'KALANDRA_'))}
    env['KALANDRA_WORKERS'] = str(workers)
    if case.get('cudaDiagnostics'):
        env['KALANDRA_CUDA_DIAGNOSTICS'] = '1'
    bundle_arguments = case.get('bundleArguments', ())
    # The standalone C++ probes use narrow argv on Windows. Give them ASCII
    # relative resource paths. Public applications still run from scratch.
    working_directory = binary.parent if bundle_arguments else scratch
    arguments = [os.path.relpath(bundle/arg, working_directory) if i in bundle_arguments else arg
                 for i, arg in enumerate(case.get('arguments', ()))]
    stdout = stderr = b''
    code = None
    error = None
    started = time.perf_counter_ns()
    try:
        result = subprocess.run([binary, *arguments], cwd=working_directory,
                                env=env, capture_output=True, timeout=180)
        code = result.returncode
        stdout, stderr = result.stdout, result.stderr
    except subprocess.TimeoutExpired as failure:
        stdout, stderr = failure.stdout or b'', failure.stderr or b''
        error = 'timed out'
    except OSError as failure:
        error = str(failure)
    elapsed = (time.perf_counter_ns() - started) / 1e6
    # The Windows CRT prints CRLF. Only translate the final line ending so
    # embedded content, including literal carriage returns and NULs, is exact.
    normalized = stdout[:-2] + b'\n' if stdout.endswith(b'\r\n') else stdout
    diagnostic = stderr.decode('utf-8', errors='replace').replace('\r\n', '\n')
    if 'stderrPrefix' in case:
        matches = diagnostic.startswith(case['stderrPrefix'])
    else:
        matches = diagnostic == case.get('stderr', '')
    if 'stdoutPrefix' in case:
        output_matches = normalized.startswith(case['stdoutPrefix'].encode('ascii'))
    else:
        output_matches = normalized == bytes.fromhex(case['stdout'])
    passed = error is None and code == case.get('exit', 0) and output_matches and matches
    return {'path': case['path'], 'arguments': case.get('arguments', []), 'missing': case.get('missing'),
            'workers': workers, 'passed': passed, 'exit': code, 'error': error, 'wallMs': elapsed,
            'stdoutBytes': len(stdout), 'stdoutSHA256': hashlib.sha256(stdout).hexdigest(),
            'stdoutHex': stdout.hex() if len(stdout) <= 256 or not passed else None,
            'stderr': diagnostic}


def open_run(args):
    identity = hardware()
    manifest_path = args.bundle/'frozen.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest['format'] != 1 or not manifest['cases']:
        raise ValueError('Invalid qualification manifest')
    verify(args.bundle, manifest)
    args.output.mkdir(parents=True, exist_ok=False)
    write_report(args.output/'run.json', {
        **identity, 'startedAt': datetime.now(timezone.utc).isoformat(),
        'revision': manifest['revision'], 'compilers': manifest['compilers'],
        'frozenSHA256': digest(manifest_path),
    })
    return manifest


def replay(args):
    manifest = open_run(args)
    failures = executions = 0
    with tempfile.TemporaryDirectory(dir=args.output) as temporary:
        scratch = Path(temporary)/'application data λ'
        scratch.mkdir()
        with (args.output/'results.jsonl').open('x', encoding='utf-8') as log:
            for case in manifest['cases']:
                for workers in case.get('workers', WORKERS):
                    record = run_case(args.bundle, case, workers, scratch)
                    log.write(json.dumps(record) + '\n')
                    log.flush()
                    executions += 1
                    failures += not record['passed']
                print(f'{executions} runs, {failures} failures: {case["path"]}', flush=True)
    verify(args.bundle, manifest)
    write_report(args.output/'summary.json', {'executions': executions, 'failures': failures})
    return int(failures != 0)


def cpu_counters():
    idle, kernel, user = (ctypes.c_uint64() for _ in range(3))
    if not ctypes.windll.kernel32.GetSystemTimes(
        ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)
    ):
        raise ctypes.WinError()
    return idle.value, kernel.value + user.value


def idle_sample():
    before = cpu_counters()
    time.sleep(1)
    after = cpu_counters()
    cpu = 100 * (1 - (after[0] - before[0]) / (after[1] - before[1]))
    row = subprocess.check_output([
        'nvidia-smi', '--query-gpu=utilization.gpu,clocks.current.sm,temperature.gpu',
        '--format=csv,noheader,nounits',
    ], text=True, timeout=20).strip()
    gpu, clock, temperature = (int(part.strip()) for part in row.split(','))
    return {'guestCpuPercent': cpu, 'gpuPercent': gpu, 'smMHz': clock, 'gpuCelsius': temperature,
            'idle': cpu <= 12.5 and gpu <= 2}


def measure(args):
    previous = args.correctness.resolve()
    correctness = json.loads((previous/'summary.json').read_text(encoding='utf-8'))
    previous_run = json.loads((previous/'run.json').read_text(encoding='utf-8'))
    if correctness['failures'] or previous_run['frozenSHA256'] != digest(args.bundle/'frozen.json'):
        raise RuntimeError('Benchmark requires a passing correctness run for this frozen bundle')
    manifest = open_run(args)
    run_identity = json.loads((args.output/'run.json').read_text(encoding='utf-8'))
    expected_runs = sum(len(case.get('workers', WORKERS)) for case in manifest['cases'])
    if correctness['executions'] != expected_runs:
        raise RuntimeError('Correctness results are incomplete')
    if run_identity['gpu'] != previous_run['gpu']:
        raise RuntimeError('GPU or driver changed since correctness testing')

    indexed = {case['path']: case for case in manifest['cases'] if 'arguments' not in case
               and 'missing' not in case}
    report = {'scope': 'Windows VM wall time per fresh process, including startup and transfers',
              'controls': [], 'comparisons': [], 'blocks': 5, 'quartetsPerBlock': 3,
              'stabilityTolerance': 0.10}

    with tempfile.TemporaryDirectory(dir=args.output) as scratch:
        with (args.output/'samples.jsonl').open('x', encoding='utf-8') as log:
            for backend in BACKENDS:
                for shape in ('128x32x128', '129x17x128'):
                    a, b = [indexed[f'{backend}/f32-{shape}-{policy}/program.exe']
                            for policy in ('fixed-tree', 'optimized')]
                    for phase, pair in (('control', (a, a)), ('comparison', (a, b))):
                        blocks = []
                        for index in range(report['blocks']):
                            load = idle_sample()
                            block = {'phase': phase, 'backend': backend, 'shape': shape,
                                     'block': index, 'load': load}
                            if load['idle']:
                                # Keep the first-launch/JIT effects out of both arms.
                                times = ([], [])
                                for quartet in range(report['quartetsPerBlock']):
                                    order = (0, 1, 1, 0) if (index + quartet) % 2 == 0 else (1, 0, 0, 1)
                                    for side in order:
                                        sample = run_case(args.bundle, pair[side], 1, Path(scratch))
                                        log.write(json.dumps({**block, **sample, 'side': side}) + '\n')
                                        log.flush()
                                        if sample['passed']:
                                            times[side].append(sample['wallMs'])
                                if all(len(arm) == 2 * report['quartetsPerBlock'] for arm in times):
                                    block['ratio'] = statistics.median(times[1]) / statistics.median(times[0])
                            else:
                                log.write(json.dumps({**block, 'rejected': 'background load'}) + '\n')
                                log.flush()
                            blocks.append(block)
                        ratios = [block['ratio'] for block in blocks if 'ratio' in block]
                        median = statistics.median(ratios) if ratios else None
                        target = 1 if phase == 'control' else median
                        report[phase + 's'].append({
                            'backend': backend, 'shape': shape, 'blocks': blocks, 'ratio': median,
                            'passed': len(ratios) == report['blocks'] and
                            all(abs(ratio / target - 1) <= report['stabilityTolerance'] for ratio in ratios),
                        })
    verify(args.bundle, manifest)
    report['accepted'] = all(item['passed'] for key in ('controls', 'comparisons') for item in report[key])
    write_report(args.output/'summary.json', report)
    return int(not report['accepted'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    stages = parser.add_subparsers(dest='stage', required=True)
    build_parser = stages.add_parser('prepare')
    build_parser.add_argument('--revision', required=True)
    build_parser.add_argument('--cuda', type=Path, required=True)
    build_parser.add_argument('--vulkan', type=Path, required=True)
    build_parser.add_argument('--bundle', type=Path, required=True)
    replay_parser = stages.add_parser('replay')
    replay_parser.add_argument('--bundle', type=Path, required=True)
    replay_parser.add_argument('--output', type=Path, required=True)
    timing_parser = stages.add_parser('measure')
    timing_parser.add_argument('--bundle', type=Path, required=True)
    timing_parser.add_argument('--correctness', type=Path, required=True)
    timing_parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.bundle = args.bundle.resolve()
    if args.stage == 'prepare':
        for backend in BACKENDS:
            setattr(args, backend, getattr(args, backend).resolve())
        prepare(args)
        return 0
    return measure(args) if args.stage == 'measure' else replay(args)


if __name__ == '__main__':
    raise SystemExit(main())
