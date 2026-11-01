"""Compile the pinned BSD-3 C++ SIMT catalog; no GPU execution or installation."""
import argparse
import importlib.util
import json
from pathlib import Path
from nvrtc import compile_ptx
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--nvrtc', required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cache', type=Path, required=True)
p.add_argument('--sdk-include', type=Path, required=True)
p.add_argument('--archive', type=Path)
p.add_argument('--cccl-include', type=Path, required=True)
p.add_argument('--crt-include', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2] / 'cuda/third_party/cutlass'
spec = importlib.util.spec_from_file_location('cutlass_headers', root / 'provision.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
include = module.provision(a.cache, a.archive)
options = ['--device-as-default-execution-space', '--include-path=' + str(include),
           '--include-path=' + str(a.sdk_include), '--include-path=' + str(a.cccl_include), '--include-path=' + str(a.crt_include)]
metadata = compile_ptx((root / 'Matmul.cu').read_bytes(), a.output,
                       fmad=True, library=a.nvrtc, extra_options=options)
metadata.update(upstream=module.PIN, productionResource=True, gpuExecution=False)
a.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
