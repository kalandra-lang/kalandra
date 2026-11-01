"""Compile the audited MIT warp-tile resources using an existing NVRTC library.

CPU-only compilation: no GPU context, driver reset, package installation or run.
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--nvrtc',default='/usr/local/cuda/lib64/libnvrtc.so.13')
p.add_argument('--chip',choices=('80','120'),default='80')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[2]/'cuda/third_party/sgemm-cuda'
spec=importlib.util.spec_from_file_location('sgemm_adapter',root/'adapt.py');adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
source=adapter.source().encode()
from nvrtc import compile_ptx
metadata=compile_ptx(source,a.output,a.chip,True,a.nvrtc)
metadata.update(upstream=json.loads((root/'upstream.json').read_text()),productionResource=True,gpuExecution=False)
a.output.with_suffix('.json').write_text(json.dumps(metadata,indent=2)+'\n')
print('PASS: full-float32 resources compiled; no GPU execution or performance claim')
