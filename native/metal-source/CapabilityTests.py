"""Keep the public Metal capability matrix tied to valid executable source probes.

This is source admission and artifact validation, not new device conformance.
Existing numerical/runtime qualification is linked separately by each supported row.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent
REPO = SOURCE.parents[1]
MATRIX = SOURCE / "capabilities.json"
DOC = REPO / "docs/design/native-gpu-capabilities.md"
START, END = "<!-- capability-matrix:start -->", "<!-- capability-matrix:end -->"


def load():
    matrix = json.loads(MATRIX.read_text())
    assert matrix['format'] == 1
    ids, fixtures = set(), set()
    for row in matrix['rows']:
        assert re.fullmatch(r'[a-z][a-z0-9-]+', row['id']) and row['id'] not in ids
        ids.add(row['id'])
        assert row['scope'] in ('GPU', 'Host') and row['status'] in ('supported', 'rejected')
        assert row['fixtures']
        assert all('|' not in row[key] and '\n' not in row[key] for key in ('feature', 'notes'))
        for fixture in row['fixtures']:
            path = (SOURCE / fixture).resolve()
            assert path.is_relative_to(SOURCE) and path.suffix == '.k' and path.is_file()
            assert fixture not in fixtures, f'duplicate fixture: {fixture}'
            fixtures.add(fixture)
        if row['status'] == 'supported':
            assert row['evidence']
            for evidence in row['evidence']:
                path = (REPO / 'native/metal-proof/evidence' / evidence).resolve()
                assert path.is_relative_to(REPO / 'native/metal-proof/evidence') and path.is_file(), evidence
        else:
            assert row['diagnostic'].startswith('G')
    # Each dedicated negative probe must be part of the visible contract.
    assert {str(p.relative_to(SOURCE)) for p in (SOURCE / 'capabilities').glob('*.k')} <= fixtures
    return matrix


def table(matrix):
    lines = ['| Feature | Runs in | Status / boundary | Checks |',
             '| --- | --- | --- | --- |']
    for row in matrix['rows']:
        fixture = row['fixtures'][0]
        links = [f'[source](../../native/metal-source/{fixture})']
        if len(row['fixtures']) > 1:
            links[0] += f" (+{len(row['fixtures'])-1} additional probes)"
        links.extend(f'[device evidence](../../native/metal-proof/evidence/{proof})'
                     for proof in row.get('evidence', []))
        status = 'Supported' if row['status'] == 'supported' else 'Not admitted'
        lines.append(f"| {row['feature']} | {row['scope']} | **{status}.** {row['notes']} | {', '.join(links)} |")
    return '\n'.join(lines)


def run(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
    assert not any(s in result.stderr for s in ('ERROR: AddressSanitizer', 'runtime error:', 'WARNING: ThreadSanitizer')), result.stderr
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler')
    parser.add_argument('--checker')
    parser.add_argument('--root')
    parser.add_argument('--write-docs', action='store_true')
    args = parser.parse_args()
    matrix = load()
    doc = DOC.read_text()
    before, tail = doc.split(START)
    _, after = tail.split(END)
    rendered = before + START + '\n\n' + table(matrix) + '\n\n' + END + after
    if args.write_docs:
        DOC.write_text(rendered)
        return
    assert doc == rendered, 'GPU capability table is stale; run CapabilityTests.py --write-docs'
    assert args.compiler and args.checker and args.root
    root = Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    outcomes = []
    with tempfile.TemporaryDirectory(dir=root) as temporary:
        work = Path(temporary)
        for row in matrix['rows']:
            for index, fixture in enumerate(row['fixtures']):
                source = SOURCE / fixture
                # All probes must be valid under the current shared syntax, types
                # and placement rules; a malformed program is not a backend limit.
                typed = run([args.checker, '--placement', source])
                assert typed.returncode == 0, (fixture, typed.stdout, typed.stderr)
                destination = work / f"{row['id']}-{index}"
                if row['status'] == 'rejected':
                    destination.mkdir()
                    (destination / 'host.cpp').write_text('existing host sentinel')
                    (destination / 'kernel0.mlir').write_text('existing shader sentinel')
                    saved = {p.name: p.read_bytes() for p in destination.iterdir()}
                emitted = run([args.compiler,"--arithmetic","software", source, '-o', destination])
                if row['status'] == 'supported':
                    assert emitted.returncode == 0, (fixture, emitted.stdout, emitted.stderr)
                    for name in ('host.cpp', 'placement.ir', 'continuation.plan', 'manifest.txt'):
                        assert (destination / name).stat().st_size > 0, (fixture, name)
                    manifest = (destination / 'manifest.txt').read_text()
                    count = int(re.search(r'^kernel_count=(\d+)$', manifest, re.M)[1])
                    assert count > 0
                    shaders = sorted(p.name for p in destination.glob('kernel*.mlir')
                                     if re.fullmatch(r'kernel\d+\.mlir', p.name))
                    assert len(shaders) == count, (fixture, manifest, shaders)
                    for shader in shaders:
                        assert 'gpu.func' in (destination / shader).read_text(), (fixture, shader)
                    for entry, stage in re.findall(r'^(kernel\d+)\.reduction_stage=(kernel\d+-reduce)$', manifest, re.M):
                        assert (destination / (entry + '.mlir')).is_file()
                        assert 'gpu.func @kalandra_reduce_sum' in (destination / (stage + '.mlir')).read_text()
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                    for entry, stage in re.findall(r'^(kernel\d+)\.epilogue_stage=(kernel\d+-post)$', manifest, re.M):
                        assert (destination / (entry + '.mlir')).is_file()
                        assert re.search(r'gpu.func @kalandra_elementwise_v[23]', (destination / (stage + '.mlir')).read_text())
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                    for entry, side, stage in re.findall(r'^(kernel\d+)\.(left|right)_stage=(kernel\d+-(?:left|right))$', manifest, re.M):
                        assert (destination / (entry + '.mlir')).is_file()
                        assert re.search(r'gpu.func @kalandra_elementwise(?:_v[23])?', (destination / (stage + '.mlir')).read_text())
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                        assert re.search(r'^' + entry + r'\.' + side + r'_tensor_bindings=\d+', manifest, re.M)
                    for entry, stage in re.findall(r'^(kernel\d+)\.scalar_stage=(kernel\d+-scalar)$', manifest, re.M):
                        assert (destination / (entry + '.mlir')).is_file()
                        assert 'gpu.func @kalandra_elementwise_v2' in (destination / (stage + '.mlir')).read_text()
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                    for entry, products in re.findall(r'^(kernel\d+)\.matrix_graph_products=(\d+)$', manifest, re.M):
                        assert int(products) >= 2 or '.join_stage=' in manifest or '.graph_result=tensor' in manifest
                        assert ('gpu.func @kalandra_matmul' if int(products) else 'gpu.func @kalandra_reduce_sum') in (destination / (entry + '.mlir')).read_text()
                        assert re.search(r'(?:matmulGraph(?:ReduceSum)?|graphReductions|graphTensor)\(', (destination / 'host.cpp').read_text())
                        edges = re.search(r'^' + entry + r'\.matrix_graph_edges=([^\n]*)$', manifest, re.M)[1]
                        edges = edges.split(',') if edges else []
                        inputs = int(re.search(r'^' + entry + r'\.matrix_graph_inputs=(\d+)$', manifest, re.M)[1])
                        stages = int(re.search(r'^' + entry + r'\.matrix_graph_stages=(\d+)$', manifest, re.M)[1])
                        assert len(edges) == stages and stages >= int(products)
                        for i, edge in enumerate(edges):
                            assert all(0 <= int(v) < inputs + i for v in edge.split(':'))
                    for entry, index, stage in re.findall(r'^(kernel\d+)\.map(\d+)_stage=(kernel\d+-map\d+)$', manifest, re.M):
                        assert re.search(r'gpu.func @kalandra_elementwise(?:_v[23])?', (destination / (stage + '.mlir')).read_text())
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                        assert int(re.search(r'^' + entry + r'\.map' + index + r'_tensors=(\d+)$', manifest, re.M)[1]) >= 1
                    for entry, stage, operands in re.findall(r'^(kernel\d+)\.map(\d+)_inputs=([^\n]+)$', manifest, re.M):
                        stage = int(stage)
                        operands = list(map(int, operands.split(',')))
                        inputs = int(re.search(r'^' + entry + r'\.matrix_graph_inputs=(\d+)$', manifest, re.M)[1])
                        edges = re.search(r'^' + entry + r'\.matrix_graph_edges=([^\n]+)$', manifest, re.M)[1].split(',')
                        assert len(operands) >= 3 and operands == sorted(set(operands))
                        assert all(0 <= value < inputs + stage for value in operands)
                        assert list(map(int, edges[stage].split(':'))) == [operands[0], operands[-1]]
                        physical = int(re.search(r'^' + entry + r'\.map' + str(stage) + r'_tensors=(\d+)$', manifest, re.M)[1])
                        broadcasts = re.search(r'^' + entry + r'\.reduction_broadcasts=([^\n]+)$', manifest, re.M)
                        scalar_reads = sum(int(pair.split(':')[0]) == stage for pair in broadcasts[1].split(',')) if broadcasts else 0
                        assert physical == len(operands) + scalar_reads
                    for entry, broadcasts in re.findall(r'^(kernel\d+)\.reduction_broadcasts=([^\n]+)$', manifest, re.M):
                        pairs = [tuple(map(int, item.split(':'))) for item in broadcasts.split(',')]
                        assert pairs == sorted(set(pairs))
                        roots = list(map(int, re.search(r'^' + entry + r'\.reduction_inputs=([^\n]+)$', manifest, re.M)[1].split(',')))
                        inputs = int(re.search(r'^' + entry + r'\.matrix_graph_inputs=(\d+)$', manifest, re.M)[1])
                        stages = int(re.search(r'^' + entry + r'\.matrix_graph_stages=(\d+)$', manifest, re.M)[1])
                        for stage, reduction in pairs:
                            assert 0 <= stage < stages and 0 <= reduction < len(roots)
                            assert all(root < inputs + stage for root in roots[:reduction+1])
                            ir = (destination / f'{entry}-map{stage}.mlir').read_text()
                            assert f'memref.load %a{inputs+stages+reduction}[%zero]' in ir
                    for entry in re.findall(r'^(kernel\d+)\.graph_result=tensor$', manifest, re.M):
                        roots = list(map(int, re.search(r'^' + entry + r'\.reduction_inputs=([^\n]+)$', manifest, re.M)[1].split(',')))
                        inputs = int(re.search(r'^' + entry + r'\.matrix_graph_inputs=(\d+)$', manifest, re.M)[1])
                        stages = int(re.search(r'^' + entry + r'\.matrix_graph_stages=(\d+)$', manifest, re.M)[1])
                        assert roots and stages > 0 and all(0 <= root < inputs + stages - 1 for root in roots)
                        assert not (destination / (entry + '-join.mlir')).exists()
                        assert not (destination / (entry + '-scalar.mlir')).exists()
                        assert 'graphTensor(' in (destination / 'host.cpp').read_text()
                    for entry, stage in re.findall(r'^(kernel\d+)\.join_stage=(kernel\d+-join)$', manifest, re.M):
                        assert re.search(r'gpu.func @kalandra_elementwise(?:_v[23])?', (destination / (stage + '.mlir')).read_text())
                        assert stage + '.metallib' in (destination / 'host.cpp').read_text()
                        roots = re.search(r'^' + entry + r'\.reduction_inputs=([^\n]+)$', manifest, re.M)[1].split(',')
                        slots = int(re.search(r'^' + entry + r'\.matrix_graph_inputs=(\d+)$', manifest, re.M)[1]) + int(re.search(r'^' + entry + r'\.matrix_graph_stages=(\d+)$', manifest, re.M)[1])
                        assert len(roots) >= 2 and all(0 <= int(root) < slots for root in roots)
                    outcomes.append({'fixture': fixture, 'status': 'supported', 'kernels': count})
                else:
                    assert emitted.returncode == 1, (fixture, emitted.stdout, emitted.stderr)
                    assert re.search(r'error\[' + row['diagnostic'] + r'\]', emitted.stderr), (fixture, emitted.stderr)
                    if 'message' in row:
                        assert row['message'] in emitted.stderr, (fixture, emitted.stderr)
                    assert saved == {p.name: p.read_bytes() for p in destination.iterdir()}, fixture
                    fresh = work / f"{row['id']}-{index}-fresh"
                    rejected = run([args.compiler,"--arithmetic","software", source, '-o', fresh])
                    assert rejected.returncode == 1 and not fresh.exists(), (fixture, rejected.stderr)
                    assert re.search(r'error\[' + row['diagnostic'] + r'\]', rejected.stderr)
                    if 'message' in row:
                        assert row['message'] in rejected.stderr, (fixture, rejected.stderr)
                    outcomes.append({'fixture': fixture, 'status': 'rejected', 'diagnostic': re.search(r'error\[(G\d+)\]', emitted.stderr)[1]})
    for outcome in outcomes:
        outcome['sourceSha256'] = hashlib.sha256((SOURCE / outcome['fixture']).read_bytes()).hexdigest()
    report = {'format': 1, 'matrixSha256': hashlib.sha256(MATRIX.read_bytes()).hexdigest(), 'outcomes': outcomes}
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"Metal capability matrix: {len(matrix['rows'])} rows, {len(outcomes)} source probes passed")


if __name__ == '__main__':
    main()
