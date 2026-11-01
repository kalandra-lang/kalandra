"""Check pinned upstream bytes and the capabilities of the actual built shaders."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('build', type=Path)
parser.add_argument('--spirv-dis', default='spirv-dis')
args = parser.parse_args()
upstream = Path(__file__).resolve().parents[1] / 'third_party/ggml'
manifest = json.loads((upstream / 'UPSTREAM.json').read_text())
assert manifest['revision'] == 'a91eab67d8389d979d29fdf6633b2edd0718eb4b'
assert manifest['license'] == 'MIT'
for name, digest in manifest['files'].items():
    assert hashlib.sha256((upstream / name).read_bytes()).hexdigest() == digest, name
    if name != 'LICENSE':
        for include in re.findall(r'^\s*#include "([^"]+)"', (upstream / name).read_text(), re.M):
            assert include in manifest['files'], (name, include)
for shader in ('add', 'mul_mm'):
    assembly = subprocess.check_output([args.spirv_dis, str(args.build / (shader + '.spv'))], text=True)
    capabilities = re.findall(r'OpCapability (\w+)', assembly)
    assert capabilities == ['Shader'], (shader, capabilities)
    widths = re.findall(r'OpTypeFloat (\d+)', assembly)
    assert widths and set(widths) == {'32'}, (shader, widths)
print('PASS: pinned upstream hashes/includes, Shader-only capabilities, float32 arithmetic')
