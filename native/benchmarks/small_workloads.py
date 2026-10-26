"""Paired complete-pipeline comparisons, including same-binary/bundle ablations."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'fair'))
import protocol as p
import pipeline_reference as ref


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--current', type=Path, required=True)
    parser.add_argument('--baseline-bundle', type=Path, required=True)
    parser.add_argument('--current-bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sizes', type=int, nargs='+', default=[128, 256, 512, 1024, 2048])
    parser.add_argument('--modes', nargs='+', choices=['resident', 'transfers'], default=['resident', 'transfers'])
    parser.add_argument('--blocks', type=int, default=7)
    parser.add_argument('--target-ms', type=int, default=30)
    parser.add_argument('--profile-only', action='store_true', help='Separate diagnostics, excluded from acceptance timing')
    args = parser.parse_args()
    assert not args.output.exists() and not args.output.with_suffix('.md').exists()
    assert args.blocks > 0 and args.target_ms > 0 and all(n > 0 for n in args.sizes)
    assert 'KALANDRA_GPU_PROFILE' not in os.environ
    for name in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'DYLD_INSERT_LIBRARIES'):
        assert os.getenv(name) in (None, '', '0'), name
    binaries = dict(baseline=args.baseline.resolve(), current=args.current.resolve())
    bundles = dict(baseline=args.baseline_bundle.resolve(), current=args.current_bundle.resolve())
    expected = {n: ref.exact(n) for n in args.sizes}

    def invoke(cell, count, rounds, environment=None):
        version, mode, n = cell
        return p.observations(p.run([binaries[version], bundles[version], int(mode == 'transfers'),
                                     2, n, count, rounds], environment), expected[n], count, rounds)

    cells = [[v, mode, n] for v in binaries for mode in args.modes for n in args.sizes]
    result = dict(baseline_bundle=str(bundles['baseline']), current_bundle=str(bundles['current']),
                  contract='Complete projection/bias/scale/ReLU/balanced sum, fresh intermediates, '
                           'scalar readback and task cleanup. Exact dyadic checksums checked in every '
                           'warmup/calibration/timed round. Shader loading excluded; two warmup batches. '
                           'Acceptance timing disables profiling. No affinity/frequency control or desktop isolation.')
    if args.profile_only:
        result['contract'] += ' This record is profiling-only diagnostics, not acceptance timing.'
        result['profiles'] = []
        for block in range(args.blocks):
            for mode in args.modes:
                for n in args.sizes:
                    order = ('baseline', 'current') if block % 2 == 0 else ('current', 'baseline')
                    for version in order:
                        cell = [version, mode, n]
                        result['profiles'].append(dict(cell=cell, block=block, iterations=10,
                            samples=invoke(cell, 10, 5, dict(os.environ, KALANDRA_GPU_PROFILE='1'))))
    else:
        result['paired'] = p.measure_paired(cells, invoke, lambda c: expected[c[-1]],
                                           blocks=args.blocks, rounds=3, target_ms=args.target_ms)
    sources = [Path(__file__), Path(__file__).parent/'fair/PipelineGpu.cpp',
               Path(__file__).parent/'fair/protocol.py', Path(__file__).parent/'fair/pipeline_reference.py',
               p.ROOT/'native/metal/Metal.mm', p.ROOT/'native/metal-source/HardwareShaders.h']
    shaders = sorted({x for bundle in bundles.values() for x in bundle.glob('*.metal')})
    artifacts = list(binaries.values()) + sorted({x for bundle in bundles.values() for x in bundle.glob('*.metallib')})
    p.evidence(args.output, result, sources + shaders, artifacts, p.ROOT/'build/native/native/kalandra')


if __name__ == '__main__':
    main()
