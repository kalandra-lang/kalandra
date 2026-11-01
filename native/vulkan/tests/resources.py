"""Byte-for-byte resource round trip and deterministic, fail-closed generation."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--extract', type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1] / 'EmbedShaders.py'
spec = importlib.util.spec_from_file_location('embed', source)
embed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(embed)
with tempfile.TemporaryDirectory(prefix='kalandra-vulkan-resources-') as directory:
    root = Path(directory)
    subprocess.run([args.extract.resolve(), root], check=True)
    paths = sorted(root.rglob('*.spv'))
    assert len(paths) == 14
    for path in paths:
        assert path.read_bytes() == (args.build / path.relative_to(root)).read_bytes(), path
    generated = embed.generate(root, paths)
    assert generated == embed.generate(root, list(reversed(paths)))
    assert generated == (args.build / 'BuiltinShaders.cpp').read_text()
    assert str(root) not in generated and str(args.build.resolve()) not in generated
    def rejected(items):
        try:
            embed.generate(root, items)
        except ValueError:
            return
        raise AssertionError('invalid resource input accepted')
    rejected(paths[:-1])
    rejected(paths + [paths[0]])
    saved = paths[0].read_bytes()
    for data in (b'', saved[:-1], b'bad!' + saved[4:], saved[:16] + b'\1\0\0\0' + saved[20:]):
        paths[0].write_bytes(data)
        rejected(paths)
    paths[0].write_bytes(saved)
print('PASS: 14 byte-exact resources; path/order-independent generation; invalid input rejected')
