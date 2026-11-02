"""Extract pinned CUDA Driver API declarations into a private test directory; install nothing."""
import argparse
import hashlib
from pathlib import Path,PurePosixPath
import urllib.request
import zipfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
name='cuda_cudart-windows-x86_64-13.2.86-archive'
url='https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/windows-x86_64/'+name+'.zip'
archive=a.output/'source.zip'
with urllib.request.urlopen(url,timeout=120) as response:archive.write_bytes(response.read(3094422))
assert archive.stat().st_size==3094421
assert hashlib.sha256(archive.read_bytes()).hexdigest()=='024e0c1055343d9d2a7d35c90fb7695db14cc2f05bb962198eb18b92c73b32be'
with zipfile.ZipFile(archive) as source:
    # Only the architecture-neutral Driver API header. No SDK libraries/DLLs.
    data=source.read(name+'/include/cuda.h')
(a.output/'cuda.h').write_bytes(data)
print(a.output.resolve())
