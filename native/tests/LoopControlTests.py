"""Execute loop semantics and verify rejected source cannot replace outputs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from LoopCases import CASES, REJECTED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), default='debug')
    args = parser.parse_args()
    args.compiler = args.compiler.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    flags = {'debug': [], 'optimized': ['-O2'], 'sanitized': ['--sanitize'],
             'thread-sanitized': ['--thread-sanitize']}[args.variant]

    def invoke(command, **options):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120, **options)
        assert not any(word in result.stderr for word in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), (command, result.stderr)
        return result

    records = []
    for name, (text, expected) in CASES.items():
        source, output = args.output / (name + '.k'), args.output / name
        source.write_text(text)
        result = invoke([args.compiler, source, '--test-entry', 'main', '-o', output, '--emit-llvm', str(output) + '.ll', *flags])
        assert result.returncode == 0 and not result.stderr, (name, 'compile', result.returncode, result.stdout, result.stderr)
        for workers in (1, 2, 4):
            result = invoke([output.resolve()], env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
            assert result.returncode == 0 and result.stdout == expected + '\n' and not result.stderr, (name, workers, result.returncode, result.stdout, result.stderr)
        records.append(dict(case=name, output=expected, workers=[1, 2, 4]))
        print('PASS', name, flush=True)
    for name, text in REJECTED.items():
        source, output = args.output / (name + '.k'), args.output / name
        source.write_text(text)
        output.write_text('preserved output\n')
        result = invoke([args.compiler, source, '--test-entry', 'main', '-o', output, *flags])
        assert result.returncode != 0 and result.stderr, (name, 'invalid source admitted')
        assert output.read_text() == 'preserved output\n', (name, 'damaged existing output')
        records.append(dict(case=name, rejected=True))
        print('PASS rejection', name, flush=True)
    source = args.output / 'zero-step.k'
    source.write_text('fn zero()->i64:\n  return 0\nfn main()->i64:\n  for i in range(0,0,zero()):\n    i+1\n  return 42\n')
    output = args.output / 'zero-step'
    result = invoke([args.compiler, source, '--test-entry', 'main', '-o', output, *flags])
    assert result.returncode == 0, result.stderr
    result = invoke([output.resolve()])
    assert result.returncode != 0 and not result.stdout and 'fault' in result.stderr.lower(), (result.returncode, result.stdout, result.stderr)
    records.append(dict(case='zero-step', checkedFailure=True))
    (args.output / 'report.json').write_text(json.dumps(dict(variant=args.variant, cases=records), indent=2) + '\n')
    print(f'Loop control {args.variant}: {len(CASES)} executed programs, {len(REJECTED)} source rejections, zero-step checked fault passed', flush=True)


if __name__ == '__main__':
    main()
