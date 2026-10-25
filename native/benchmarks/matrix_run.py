"""Paired strict-matrix tuning: saved baseline compiler/GPU, current production code.

See the adjacent report for how to save baseline GPU artifacts before rebuilding.
All allocation, packing, cleanup and scalar readback remain inside the timer.
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
import library_reference as ref


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--baseline-compiler', type=Path, required=True)
    parser.add_argument('--baseline-gpu', type=Path, required=True)
    parser.add_argument('--baseline-gpu-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--reuse-build', action='store_true')
    args = parser.parse_args()
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    for key in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES',
                'KALANDRA_GPU_PROFILE', 'KALANDRA_FAIR_PATTERN'):
        assert os.environ.get(key) in (None, '', '0'), key + ' must be disabled for timing'
    compiler = p.ROOT / 'build/native/native/kalandra'
    runtime = compiler.parent / 'libkalandra-task-core-optimized.a'
    metal = compiler.parent / 'metal/libkalandra-metal-optimized.a'
    src = Path(__file__).resolve().parent / 'languages'
    fair = src.parent / 'fair'
    build = p.BUILD.parent / 'matrix-tuning'
    build.mkdir(exist_ok=True)
    flags = ['-std=c++23', '-O2', '-DNDEBUG', '-ffp-contract=off', '-pthread', '-Wno-override-module']
    # Same checked driver and input generators as the fair numerical suite.
    def driver(name):
        source = src / name
        import re
        text = re.sub(r'#include "([^"]+)"',
                      lambda m: '#include "' + str((source.parent / m[1]).resolve()) + '"',
                      source.read_text())
        text = '#include "' + str(fair / 'FairInputs.h') + '"\n' + text
        return text.replace('(int(i % 17) - 8) / 8.0', 'fairA<Float>(i)').replace(
            '(int(i % 13) - 6) / 8.0', 'fairB<Float>(i)')
    manifest = build / 'manifest.json'
    inputs = [Path(__file__), compiler, runtime, metal, args.baseline_compiler, args.baseline_gpu,
              *args.baseline_gpu_bundle.glob('*.metallib'), *[x for x in fair.iterdir() if x.is_file()],
              src / 'kernels.k', src / 'gpu.k', src / 'Driver.cpp', src / 'GpuDriver.cpp',
              p.ROOT / 'native/lib/Backend.cpp', p.ROOT / 'native/metal/Metal.mm',
              p.ROOT / 'native/metal-source/HardwareShaders.h']
    signature = {str(x): p.sha(x) for x in inputs}
    import json
    if args.reuse_build:
        cached = json.loads(manifest.read_text())
        assert signature == cached['signature'], 'build inputs changed'
        assert all(p.sha(x) == h for x, h in cached['hashes'].items()), 'build artifacts changed'
        p.COMMANDS.extend(cached['commands'])
        generated = list(map(Path, cached['generated']))
        artifacts = list(map(Path, cached['artifacts']))
        cpu = {tuple(k.split('|')): Path(v) for k, v in cached['cpu'].items()}
        bundle, gpu = build / 'float32/gpu-bundle', build / 'float32/gpu'
    else:
        generated, artifacts, cpu = [], [runtime, metal, args.baseline_compiler, args.baseline_gpu], {}
        for dtype in ('float32', 'float64'):
            directory = build / dtype
            directory.mkdir(exist_ok=True)
            source = directory / 'kernels.k'
            source.write_text((src / 'kernels.k').read_text().replace('float64', dtype))
            host = directory / 'Driver.cpp'
            host.write_text(driver('Driver.cpp'))
            generated += [source, host]
            macros = ['-DBENCH_FLOAT32'] if dtype == 'float32' else []
            for version, tool in (('baseline', args.baseline_compiler), ('current', compiler)):
                ir = directory / (version + '.ll')
                binary = directory / version
                p.run([tool, source, '--arithmetic', 'hardware', '--test-entry', 'matrix',
                       '--emit-llvm', ir, '-O2', '-o', str(binary) + '-standalone'])
                p.run([p.CLANG, *flags, *macros, '-DKALANDRA_KERNEL', '-DMATRIX_KERNEL',
                       host, ir, runtime, '-o', binary])
                cpu[version, dtype] = binary
                generated.append(ir)
                artifacts.append(binary)
        directory = build / 'float32'
        gpu_source, gpu_host = directory / 'gpu.k', directory / 'GpuDriver.cpp'
        gpu_source.write_text((src / 'gpu.k').read_text().replace('float64', 'float32'))
        gpu_host.write_text(driver('GpuDriver.cpp'))
        bundle, gpu = directory / 'gpu-bundle', directory / 'gpu'
        p.run([compiler, gpu_source, '--target', 'metal', '--arithmetic', 'hardware', '-O2', '-o', bundle])
        p.run([p.CLANG, *flags, '-DBENCH_FLOAT32', gpu_host, metal, runtime,
               '-framework', 'Foundation', '-framework', 'Metal', '-o', gpu])
        generated += [gpu_source, gpu_host]
        artifacts += [gpu, *bundle.glob('*.metallib'), *args.baseline_gpu_bundle.glob('*.metallib')]
        manifest.write_text(json.dumps(dict(signature=signature, commands=list(p.COMMANDS),
            generated=list(map(str, generated)), artifacts=list(map(str, artifacts)),
            cpu={'|'.join(k): str(v) for k, v in cpu.items()},
            hashes={str(x): p.sha(x) for x in generated + artifacts}), indent=2) + '\n')
    if args.build_only:
        print('Matrix artifacts prepared; timing not run', flush=True)
        return
    cells = [[v, 'cpu', dtype, n] for v in ('baseline', 'current')
             for dtype, sizes in (('float32', (128, 256, 512, 1024)), ('float64', (128, 256, 512, 1024)))
             for n in sizes]
    cells += [[v, mode, 'float32', n] for v in ('baseline', 'current')
              for mode in ('resident', 'transfers') for n in (128, 256, 512, 1024, 2048)]

    def invoke(cell, count, rounds, env=None):
        version, mode, dtype, n = cell
        if mode == 'cpu':
            command = [cpu[version, dtype], 2, n, count, rounds]
        else:
            binary, kernels = (args.baseline_gpu, args.baseline_gpu_bundle) if version == 'baseline' else (gpu, bundle)
            command = [binary, kernels, int(mode == 'transfers'), 2, n, count, rounds]
        return p.observations(p.run(command, env), ref.exact(2, n), count, rounds)

    result = p.measure_paired(cells, invoke, lambda c: ref.exact(2, c[-1]), blocks=7, rounds=3, target_ms=10)
    result['contract'] = ('CPU float32/float64 hardware and Metal hardware float32; same source expressions and exact dyadic inputs. '
                          'Fresh invocation, output/intermediate allocation, CPU packing, cleanup, and GPU scalar completion/readback included. '
                          'Resident excludes uploads; transfers includes uploads. Initialization, shader loading and compilation excluded. '
                          'Both CPU versions link the same runtime archive. No general compiler/library parity claim. '
                          'Seven mirrored ABBA/BAAB blocks; uncontrolled desktop scheduling/frequency.')
    # Separate instrumented GPU runs, never mixed with acceptance timing.
    result['gpu_profile'] = []
    env = dict(os.environ, KALANDRA_GPU_PROFILE='1')
    for n in (256, 512, 1024, 2048):
        for version in ('baseline', 'current'):
            cell = [version, 'resident', 'float32', n]
            result['gpu_profile'].append(dict(cell=cell, iterations=5, samples=invoke(cell, 5, 5, env)))
    p.evidence(args.output, result,
               [Path(__file__), fair / 'protocol.py', fair / 'library_reference.py', fair / 'FairInputs.h',
                src / 'kernels.k', src / 'gpu.k', src / 'Driver.cpp', src / 'GpuDriver.cpp',
                p.ROOT / 'native/lib/Backend.cpp', p.ROOT / 'native/metal-source/HardwareShaders.h',
                p.ROOT / 'native/metal/Metal.mm', *generated], artifacts, compiler)


if __name__ == '__main__':
    main()
