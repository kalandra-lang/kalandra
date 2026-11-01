"""Validate the exact native control normalizer with real compiler SPIR-V."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tool',type=Path,required=True)
p.add_argument('--shader',type=Path,required=True)
p.add_argument('--validator',required=True)
p.add_argument('--disassembler',required=True)
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='kalandra-f64-controls-') as directory:
    root=Path(directory)
    for denormals in (False,True):
        output=root/('denorm.spv' if denormals else 'native.spv')
        flags=['--denorm'] if denormals else []
        subprocess.run([a.tool,a.shader,output,*flags],check=True)
        subprocess.run([a.validator,'--target-env','vulkan1.2',output],check=True)
        text=subprocess.check_output([a.disassembler,output],text=True)
        assert 'SignedZeroInfNanPreserve 64' in text
        assert ('DenormPreserve 64' in text)==denormals
        assert 'RoundingModeRTE' not in text
        second=root/'second.spv'
        subprocess.run([a.tool,output,second,*flags],check=True)
        assert second.read_bytes()==output.read_bytes()
print('PASS: native float64 control modules validate, preserve ABI and are idempotent; no RTE',flush=True)
