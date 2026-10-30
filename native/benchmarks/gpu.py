#!/usr/bin/env python3
"""Build and measure strict source-generated CPU/Metal kernels without sanitizers."""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import shutil
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(command, timeout=300, env=None):
    return subprocess.check_output([str(x) for x in command], cwd=ROOT, text=True,
                                   timeout=timeout, env=env)


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=pathlib.Path, default=ROOT / 'build/native/native/kalandra')
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--suite', choices=['initial', 'selection'], default='initial')
    parser.add_argument('--baseline-metal-source', type=pathlib.Path,
                        help='Rebuild an earlier Metal.mm against the same harness; direct matmul defaults to scalar')
    parser.add_argument('--baseline-adaptive-matmul', action='store_true',
                        help='Use the current adaptive direct-matmul policy when rebuilding a matching baseline runtime')
    parser.add_argument('--baseline-dir', type=pathlib.Path,
                        help='Saved directory containing scaling and kernels/kernel{0,1}.metallib')
    parser.add_argument('--processes', type=int, default=3)
    parser.add_argument('--samples', type=int, default=7)
    parser.add_argument('--warmups', type=int, default=2)
    parser.add_argument('--smoke', action='store_true', help='One warmup and sample per cell in one process')
    args = parser.parse_args()
    if args.smoke:
        args.processes = args.samples = args.warmups = 1
    if min(args.processes, args.samples, args.warmups) < 1:
        parser.error('processes, samples and warmups must be positive')
    build = ROOT / 'benchmarks/.build/native-gpu'
    build.mkdir(parents=True, exist_ok=True)
    output = args.output or (build / 'smoke.json' if args.smoke else ROOT / f'benchmarks/results/native-gpu-{datetime.date.today().isoformat()}.json')
    if output.exists() and not args.smoke:
        parser.error('output already exists; choose a new path to preserve previous measurements')
    if args.baseline_dir and args.baseline_metal_source:
        parser.error('choose a saved baseline directory or a baseline Metal source, not both')
    if args.baseline_metal_source and not args.baseline_metal_source.is_file():
        parser.error('baseline Metal source is missing')
    if args.baseline_adaptive_matmul and not args.baseline_metal_source:
        parser.error('--baseline-adaptive-matmul requires --baseline-metal-source')
    baseline_artifacts = {}
    if args.baseline_dir:
        args.baseline_dir = args.baseline_dir.resolve()
        paths = [args.baseline_dir / 'scaling', *[args.baseline_dir / 'kernels' / f'kernel{i}.metallib' for i in [0, 1]]]
        rebuilt = {build / 'scaling', *[build / 'kernels' / f'kernel{i}.metallib' for i in [0, 1]]}
        if (not all(p.is_file() for p in paths) or
                any(p.resolve() in {q.resolve() for q in rebuilt} for p in paths)):
            parser.error('baseline must be a separate saved directory with executable and both Metal libraries')
        baseline_artifacts = {str(p): digest(p) for p in paths}
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    clang = pathlib.Path(lock['defaultLlvmRoot']) / 'bin/clang++'
    bundle = build / 'kernels'
    binary = build / 'scaling'
    commands = [[args.compiler.resolve(), ROOT / 'native/benchmarks/gpu.k',
                 '--target', 'metal', '--arithmetic', 'software', '-o', bundle, '-O2']]
    flags = ['-std=c++23', '-Wall', '-Wextra', '-Werror', '-O2', '-DNDEBUG', '-ffp-contract=off']
    objects = []
    for index, source in enumerate([ROOT / 'native/benchmarks/GpuScaling.mm', ROOT / 'native/metal/Metal.mm',
                                    ROOT / 'native/runtime/Runtime.cpp', ROOT / 'native/runtime/Tasks.cpp', bundle / 'cpu.ll']):
        obj = build / f'unit-{index}.o'
        objects.append(obj)
        extra = ['-fobjc-arc', '-fblocks', '-fno-objc-msgsend-class-selector-stubs'] if source.suffix == '.mm' else []
        if source.suffix == '.ll':
            extra += ['-Wno-override-module']
        commands.append([clang, *flags, *extra, '-c', source, '-o', obj])
    commands.append([clang, '-O2', *objects, '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
    if args.baseline_metal_source:
        args.baseline_dir = build / 'baseline-controlled'
        args.baseline_dir.mkdir(parents=True, exist_ok=True)
        baseline_objects = []
        for index, source in enumerate([ROOT / 'native/benchmarks/GpuScaling.mm', args.baseline_metal_source.resolve(),
                                       ROOT / 'native/runtime/Runtime.cpp', ROOT / 'native/runtime/Tasks.cpp', bundle / 'cpu.ll']):
            obj = args.baseline_dir / f'unit-{index}.o'
            baseline_objects.append(obj)
            extra = ['-fobjc-arc', '-fblocks', '-fno-objc-msgsend-class-selector-stubs'] if source.suffix == '.mm' else []
            if index == 0 and not args.baseline_adaptive_matmul:
                extra += ['-DKALANDRA_BENCH_SCALAR_MATMUL=1']
            if index == 1:
                extra += ['-I', ROOT / 'native/metal']
            if source.suffix == '.ll':
                extra += ['-Wno-override-module']
            commands.append([clang, *flags, *extra, '-c', source, '-o', obj])
        commands.append([clang, '-O2', *baseline_objects, '-framework', 'Foundation', '-framework', 'Metal', '-o', args.baseline_dir / 'scaling'])
    start = time.monotonic()
    for command in commands:
        run(command)
    build_seconds = time.monotonic() - start
    if args.baseline_metal_source:
        (args.baseline_dir / 'kernels').mkdir(exist_ok=True)
        for index in [0, 1]:
            shutil.copyfile(bundle / f'kernel{index}.metallib', args.baseline_dir / 'kernels' / f'kernel{index}.metallib')
        baseline_artifacts = {str(p): digest(p) for p in [args.baseline_dir / 'scaling',
            args.baseline_dir / 'kernels/kernel0.metallib', args.baseline_dir / 'kernels/kernel1.metallib', args.baseline_metal_source.resolve()]}
    env = os.environ.copy()
    for name in ['MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES']:
        if env.get(name) not in (None, '', '0'):
            parser.error(f'{name} is enabled; performance runs require validation/injection disabled')
        env.pop(name, None)
    trials, baseline_trials, order = [], [], []
    for process in range(args.processes):
        variants = [('current', binary, bundle, trials)]
        if args.baseline_dir:
            variants.insert(0, ('baseline', args.baseline_dir / 'scaling', args.baseline_dir / 'kernels', baseline_trials))
            if process % 2:
                variants.reverse()
        for name, executable, kernels, observations in variants:
            print(f'Native GPU {name} process {process + 1}/{args.processes}', flush=True)
            lines = [json.loads(line) for line in run([executable, kernels, args.warmups, args.samples, process, *(['selection'] if args.suite == 'selection' else [])], env=env).splitlines()]
            assert len(lines) == 1 + 9 * 4 * args.samples
            observations.append(dict(metadata=lines[0]['metadata'], samples=lines[1:]))
            order.append(dict(variant=name, process=process))
    assert all(t['metadata']['registry_id'] == trials[0]['metadata']['registry_id'] for t in trials + baseline_trials)

    def summarize(trials):
        summary = []
        for workload in sorted({r['workload'] for r in trials[0]['samples']}):
            for mode in ['cpu', 'device', 'resident', 'transfers']:
                rows = [[r for r in trial['samples'] if r['workload'] == workload and r['mode'] == mode] for trial in trials]
                assert all(len(r) == args.samples for r in rows)
                cell = dict(workload=workload, mode=mode)
                for metric in ['wall_ms', 'gpu_ms', 'upload_ms', 'compute_ms', 'download_ms', 'retire_ms']:
                    values = [[r[metric] for r in process if r[metric] is not None] for process in rows]
                    medians = [statistics.median(v) for v in values if v]
                    cell[metric] = statistics.median(medians) if medians else None
                    cell[metric + '_process_medians'] = medians
                    cell[metric + '_min'] = min((min(v) for v in values if v), default=None)
                    cell[metric + '_max'] = max((max(v) for v in values if v), default=None)
                summary.append(cell)
        return summary

    summary = summarize(trials)
    sources = ['native/benchmarks/GpuScaling.mm', 'native/benchmarks/gpu.py', 'native/benchmarks/gpu.k',
               'native/metal/Metal.h', 'native/metal/Metal.mm', 'native/metal/MatmulLayout.h', 'native/runtime/Tensor.h', 'native/runtime/Runtime.h', 'native/runtime/Runtime.cpp',
               'native/runtime/Tasks.h', 'native/runtime/Tasks.cpp', 'native/lib/Backend.cpp', 'native/lib/CheckedABI.cpp',
               'native/metal-source/Compiler.cpp', 'native/metal-source/MatmulShader.h', 'native/metal-source/MatmulTiles.h', 'native/metal-source/Build.cpp',
               'native/lib/StrictFloat.cpp', 'native/metal-proof/FloatOracle.h', 'native/metal-proof/Strict.cpp', 'native/metal-proof/Extract.cpp', 'native/toolchain-proof/toolchain.json']
    artifacts = [binary, args.compiler, bundle / 'cpu.ll', bundle / 'cpu.mlir']
    for index in [0, 1]:
        artifacts += [bundle / f'kernel{index}.{suffix}' for suffix in ['mlir', 'lowered.mlir', 'spv', 'metallib']]
    for tile in ['rows2', 'quad']:
        artifacts += [bundle / f'kernel0-{tile}.{suffix}' for suffix in ['mlir', 'lowered.mlir', 'spv', 'metallib']]
    machine = {}
    for key in ['machdep.cpu.brand_string', 'hw.logicalcpu', 'hw.perflevel0.logicalcpu', 'hw.perflevel1.logicalcpu', 'hw.memsize']:
        machine[key] = run(['sysctl', '-n', key]).strip()
    result = dict(arithmetic='software', timestamp=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  platform=platform.platform(), machine=machine, revision=run(['git', 'rev-parse', 'HEAD']).strip(),
                  worktree_status=run(['git', 'status', '--short']),
                  protocol=dict(suite=args.suite, controlled_scalar_baseline=bool(args.baseline_metal_source) and not args.baseline_adaptive_matmul,
                                controlled_adaptive_baseline=bool(args.baseline_metal_source) and args.baseline_adaptive_matmul, processes=args.processes, warmups=args.warmups, samples=args.samples,
                                shuffled_modes_each_round=True, shuffled_workloads_each_process=True,
                                workers=1, managed_max_bytes=(512 if args.suite == 'selection' else 64) * 1024 * 1024, optimized=True, sanitizers=False, metal_validation=False, jit=False,
                                gpu_timing='MTLCommandBuffer GPUStartTime/GPUEndTime: compute command only, no validation readback',
                                cpu_timing='checked C-interface call plus Invocation construction; output retained for validation',
                                task_timing='group admission, execution, observation and retirement; output retained for validation',
                                transfers='existing host tensors: upload(s), compute, matrix download, group retirement; reduction returns scalar',
                                setup_excluded=['compilation', 'library/pipeline creation', 'pool/runtime creation', 'input/reference construction', 'resident uploads', 'direct Metal buffer allocation'],
                                post_timing=['numerical validation', 'returned output deallocation'],
                                source_continuations_measured=False),
                  build_seconds=build_seconds, build_commands=[[str(x) for x in c] for c in commands],
                  clang=run([clang, '--version']).strip(),
                  source_sha256={name: digest(ROOT / name) for name in sources},
                  process_order=order, artifacts_sha256={str(p.relative_to(ROOT)): digest(p) for p in artifacts}, trials=trials, summary=summary)
    if args.baseline_dir:
        assert all(digest(p) == value for p, value in baseline_artifacts.items())
        result['baseline'] = dict(directory=str(args.baseline_dir), artifacts_sha256=baseline_artifacts,
                                  trials=baseline_trials, summary=summarize(baseline_trials))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + '\n')
    for row in summary:
        print(f"{row['workload']:20} {row['mode']:10} wall={row['wall_ms']:.4f} ms GPU={row['gpu_ms']}")
    print(f'Raw results: {output}')


if __name__ == '__main__':
    main()
