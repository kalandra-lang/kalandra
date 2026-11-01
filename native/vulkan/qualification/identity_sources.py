"""Build complete identity tasks for independent bitwise copy/ownership probes."""
import argparse
import json
import os
import struct
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--execute', action='store_true')
a = p.parse_args()
a.output = a.output.resolve()
records = []
for policy in ('hardware', 'software'):
    root = a.output/policy
    root.mkdir(parents=True, exist_ok=True)
    for numeric in ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64'):
        source = root/(numeric+'.k')
        source.write_text(f'''fn identity(x:Tensor[{numeric},?])->Tensor[{numeric},?]:
  x
fn main()->Tensor[{numeric},?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[{numeric},3]=tensor([0,1,2])
    resident=try await transfer(values,gpu)
    copied=try await spawn identity(resident) on gpu
    try await transfer(copied,devices.cpu())
''')
        output = root/numeric
        result = subprocess.run([a.compiler.resolve(), '--target', 'vulkan', '--arithmetic', policy,
                                 '-O2', source, '-o', output], capture_output=True, text=True, timeout=180)
        (root/(numeric+'.log')).write_text(result.stdout+result.stderr)
        assert not result.returncode, (numeric, policy, result.stderr[-4000:])
        assert (output/'kernel0.spv').is_file()
        if a.execute:
            width = 32 if numeric == 'float32' else 64 if numeric == 'float64' else int(numeric.removeprefix('u').removeprefix('int'))
            bits = lambda value: int.from_bytes(struct.pack('<f' if width == 32 else '<d', value), 'little') if numeric.startswith('float') else value
            expected = '['+','.join(f'{bits(value):0{width//4}x}' for value in (0, 1, 2))+']\n'
            for device in ('nvidia', 'intel'):
                for workers in (1, 2, 4):
                    result = subprocess.run([output/'program'], capture_output=True, text=True, timeout=60,
                        env=os.environ | {'VK_DRIVER_FILES': '/usr/share/vulkan/icd.d/'+device+'_icd.x86_64.json', 'KALANDRA_WORKERS': str(workers)})
                    records.append({'type': numeric, 'arithmetic': policy, 'device': device, 'workers': workers,
                                    'status': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
                    (a.output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
                    assert not result.returncode and result.stdout == expected and not result.stderr, records[-1]
        print('PASS:', numeric, policy, 'complete identity source'+(' physical execution' if a.execute else ' compilation'), flush=True)
print('PASS: twenty complete identity tasks;', len(records), 'physical executions', flush=True)
