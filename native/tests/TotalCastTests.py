"""APFloat qualification of total hardware casts, including every 8/16-bit value."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--tensor-only', action='store_true')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    llvm = a.clang.resolve().parents[1]
    def run(argv):
        r = subprocess.run(list(map(str, argv)), text=True, capture_output=True, timeout=180)
        assert r.returncode == 0, (argv, r.returncode, r.stdout, r.stderr)
        return r.stdout.strip()
    variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
                ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
                                           '-fno-sanitize-recover=all'])]
    if sys.platform != 'win32':
        variants.append(('thread-sanitized', '--thread-sanitize', ['-O1', '-g', '-fsanitize=thread']))
    integers = ['int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64']
    pairs = [(source, destination) for source in integers for destination in ['float32', 'float64']]
    pairs.append(('float32', 'float64'))
    types = integers + ['float32', 'float64']
    results = []
    for variant, flag, flags in variants:
        runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
            'kalandra-native-runtime' + ('-' + variant if 'sanitized' in variant else '') +
            ('.lib' if sys.platform == 'win32' else '.a'))
        link = ([f'-L{llvm / "lib"}', f'-Wl,-rpath,{llvm / "lib"}', '-lLLVM']
                if sys.platform != 'win32' else [llvm / 'lib/LLVM.lib', '-fms-runtime-lib=static'])
        for source, destination in ([] if a.tensor_only else pairs):
            name = f'{variant}-{source}-{destination}'
            path = a.output / (name + '.k')
            path.write_text(f'fn convert(value:{source})->{destination}:\n  {destination}(value)\nfn main()->int:\n  0\n')
            binary = a.output / name
            ir = binary.with_suffix('.ll')
            policy = 'hardware-with-software-fallback' if variant == 'sanitized' else 'hardware'
            run([a.compiler, path, '--test-entry', 'convert', '--arithmetic', policy, flag,
                 '--emit-llvm', ir, '-o', str(binary) + '-standalone'])
            text = '\n'.join(line for line in ir.read_text().splitlines()
                             if not line.startswith('target datalayout'))
            operation = 'fpext' if source == 'float32' else 'uitofp' if source.startswith('u') else 'sitofp'
            assert operation in text and 'i128' not in text, name
            run([a.clang, '-std=c++23', '-Wno-override-module', '-ffp-contract=off', '-pthread',
                 *flags, '-DTOTAL_HARDWARE_CAST', f'-DSOURCE_TYPE={types.index(source)}',
                 f'-DDESTINATION_TYPE={types.index(destination)}', '-isystem', llvm / 'include',
                 root / 'native/tests/ConversionRuntimeTests.cpp', ir, runtime, *link, '-o', binary])
            result = run([binary])
            results.append(dict(variant=variant, source=source, destination=destination, result=result))
            print(variant + ': ' + result, flush=True)
        for source, destination in [('int8', 'float32'), ('uint64', 'float64'), ('float32', 'float64')]:
            for rank in range(4):
                name = f'{variant}-tensor-{source}-{destination}-{rank}'
                path = a.output / (name + '.k')
                shape = ',?' * rank
                path.write_text(f'fn convert(value:Tensor[{source}{shape}])->Tensor[{destination}{shape}]:\n  {destination}(value)\nfn main()->int:\n  0\n')
                binary = a.output / name
                ir = binary.with_suffix('.ll')
                run([a.compiler, path, '--test-entry', 'convert', flag, '--emit-llvm', ir,
                     '-o', str(binary) + '-standalone'])
                run([a.clang, '-std=c++23', '-Wno-override-module', '-ffp-contract=off', '-pthread',
                     *flags, '-DTOTAL_HARDWARE_CAST', f'-DSOURCE_TYPE={types.index(source)}',
                     f'-DDESTINATION_TYPE={types.index(destination)}', f'-DTENSOR_RANK={rank}',
                     '-isystem', llvm / 'include', root / 'native/tests/TensorConversionRuntimeTests.cpp',
                     ir, runtime, *link, '-o', binary])
                result = run([binary])
                results.append(dict(variant=variant, source=source, destination=destination,
                                    rank=rank, result=result))
                print(variant + ': ' + result, flush=True)
    (a.output / 'qualification.json').write_text(json.dumps(results, indent=2) + '\n')

if __name__ == '__main__':
    main()
