"""Reduced-precision instructions are forbidden; unused type tags are harmless."""
from pathlib import Path
import subprocess
import sys
import tempfile
entry = 'kalandra_numeric_v1_matmul_float32_catalog_cuda_desc_v1'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    source, output = root/'input.ptx', root/'output.ptx'
    base = '.version 9.2\n.target sm_80\n.visible .entry ' + entry + '() { ret; }\n'
    for token, allowed in [('__kind_tf32', True), ('.tf32', False), ('.f16', False),
                           ('.bf16', False), ('mma.sync', False), ('wgmma.', False),
                           ('tcgen05.', False), ('.extern .func', False)]:
        source.write_text(base + token + '\n')
        result = subprocess.run([sys.argv[1], source, output, 'matmul-catalog', 'float32', '80'])
        assert (result.returncode == 0) == allowed, token
print('PASS: full-FP32 wrapper instruction guards and unused type tag')
