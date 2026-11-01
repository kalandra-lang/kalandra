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
    assert 'RelaxedPrecision' not in assembly, (shader, 'operand precision must remain float32')
for path in sorted((args.build/'transforms').glob('*.spv')):
    assembly=subprocess.check_output([args.spirv_dis,str(path)],text=True)
    kind,numeric=path.stem.split('-')
    assert f'"kalandra_tensor_transform_v1_{kind}_{numeric}_vk_groups_v1"' in assembly,path
    assert re.search(r'OpExecutionMode %\w+ LocalSize 256 1 1',assembly),path
    if kind!='transpose':
        assert 'OpCapability SignedZeroInfNanPreserve' in assembly,path
        assert re.search(r'OpExecutionMode %\w+ SignedZeroInfNanPreserve 32',assembly),path
        assert 'RoundingModeRTE' not in assembly,path
        assert not re.search(r'OpExtInst .* Fma\b',assembly),path
        decorated=set(re.findall(r'OpDecorate (%\w+) NoContraction',assembly))
        operations=set(re.findall(r'(%\w+) = OpF(?:Add|Sub|Mul|Div)\b',assembly))
        assert operations and operations<=decorated,(path,operations-decorated)
assert len(list((args.build/'transforms').glob('*.spv')))==12
print('PASS: pinned upstream hashes/includes, primitive/transform ABI, native precision decorations')
