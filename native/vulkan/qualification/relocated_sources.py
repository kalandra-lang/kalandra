"""Qualify complete Vulkan executables launched independently of build/source paths.

The existing private builder is used; this is not installed compiler packaging.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
for name in ('build', 'llvm-root', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
parser.add_argument('--cxx', type=Path, default=Path('/usr/bin/clang++'))
args = parser.parse_args()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[3]
cast = args.output / 'cast.k'
cast.write_text('''fn work(x:Tensor[float64,?])->Tensor[float32,?]:
  float32(x)
fn main()->Tensor[float32,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    x:Tensor[float64,3]=tensor([-3.25,0.5,5.75])
    resident=try await transfer(x,gpu)
    pending=spawn work(resident) on gpu
    first=try await pending
    cached=try await pending
    try await transfer(cached,devices.cpu())
''')
cases = [('placed', repo/'native/metal-source/placed.k', 'hardware', [5, 12, 21]),
         ('cpu-spawn', repo/'native/metal-source/cpu-spawn.k', 'hardware', [52, 54, 64, 66]),
         ('fallback-cast', cast, 'hardware-with-software-fallback', [-3.25, .5, 5.75])]
records = []
for name, source, policy, values in cases:
    original = args.output / name
    command = [sys.executable, Path(__file__).with_name('source_program.py'),
               '--build', args.build, '--llvm-root', args.llvm_root, '--cxx', args.cxx,
               '--source', source, '--output', original, '--arithmetic', policy]
    if policy == 'hardware-with-software-fallback':
        command.append('--test-disable-native-float64')
    built = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=180)
    (args.output/(name+'-build.log')).write_text(built.stdout+built.stderr)
    assert built.returncode == 0, (name, built.stderr[-4000:])
    expected = '['+','.join(f'{struct.unpack("<I", struct.pack("<f", value))[0]:08x}' for value in values)+']\n'
    with tempfile.TemporaryDirectory(prefix='kalandra-relocated-source-') as directory:
        root = Path(directory).resolve()
        destination = root/'space café λ'
        destination.mkdir()
        executable = destination/'program'
        shutil.copy2(original/'program', executable)
        for path in original.glob('*.spv'):
            shutil.copyfile(path, destination/path.name)
        link = root/'alias'
        link.symlink_to(executable)
        hidden = args.output/(name+'-hidden')
        assert not hidden.exists()
        original.rename(hidden)
        try:
            for label, icd in [('nvidia', 'nvidia_icd.x86_64.json'), ('intel', 'intel_icd.x86_64.json')]:
                for workers in (1, 2, 4):
                    environment = os.environ | {'VK_DRIVER_FILES': '/usr/share/vulkan/icd.d/'+icd,
                                                  'KALANDRA_WORKERS': str(workers),
                                                  'PATH': str(destination)+os.pathsep+os.environ.get('PATH', '')}
                    for launch, command in [('absolute', [executable]),
                                            ('relative', [str(executable.relative_to(root))]),
                                            ('path', ['program']), ('symlink', [link]),
                                            ('explicit-directory', [executable, destination])]:
                        result = subprocess.run(command, cwd=root, env=environment,
                                                capture_output=True, text=True, timeout=60)
                        record = {'case': name, 'device': label, 'workers': workers,
                                  'launch': launch, 'status': result.returncode,
                                  'stdout': result.stdout, 'stderr': result.stderr}
                        records.append(record)
                        (args.output/'results.json').write_text(json.dumps(records, indent=2)+'\n')
                        assert not result.returncode and not result.stderr and result.stdout == expected, record
        finally:
            assert not original.exists(), f'concurrent writer; original retained at {hidden}'
            hidden.rename(original)
    print('PASS:', name, 'relocated source executable, both GPUs workers1/2/4, five launch modes', flush=True)
print('PASS:', len(records), 'complete-source executions with original artifact directories absent', flush=True)
