"""Generate and run independent APFloat four-operation edge/random conformance.

No host floating arithmetic oracle, installation or performance timing.
"""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--execute',action='store_true',required=True)
a=p.parse_args();a.build=a.build.resolve();a.output=a.output.resolve()
a.output.mkdir(parents=True,exist_ok=False)
fixture=Path(__file__).parents[1].joinpath('tests/elementwise.mlir').read_text()
original='''        %product = arith.mulf %a, %b : f32
        %sum = arith.addf %product, %a : f32'''
assert original in fixture
for width in (32,64):
 for operation in ('add','sub','mul','div'):
  source=a.output/f'f{width}-{operation}.mlir'
  source.write_text(fixture.replace(original,f'        %sum = arith.{operation}f %a, %b : f32').replace('f32','f'+str(width)))
  for policy in ('hardware','software','hardware-with-software-fallback'):
   subprocess.run([a.build/'native/cuda-source/kalandra-cuda-lower',source,
       a.output/f'f{width}-{operation}-{policy}.ptx',policy,'sm_80'],check=True,capture_output=True,timeout=90)
subprocess.run([a.build/'native/cuda-source/kalandra-cuda-float-tests',a.output],check=True,timeout=240)
