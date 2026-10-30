"""Static tensor shapes must survive hardware matrix/map buffer extraction."""
import argparse
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)

def run(args):
    r = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=240)
    assert r.returncode == 0, (args, r.returncode, r.stdout, r.stderr)
    return r.stdout.strip()

variants = ['-O0', '-O2', '--sanitize']
if sys.platform != 'win32':
    variants.append('--thread-sanitize')
for numeric in ('float32', 'float64'):
    cases = [
        ('documentation', f'''fn main()->{numeric}:
  a:Tensor[{numeric},1,2]=tensor([[2,3]])
  b:Tensor[{numeric},2,1]=tensor([[4],[5]])
  reduceSum(matmul(a,b))
''', '23'),
        ('rectangular', f'''fn main()->{numeric}:
  a:Tensor[{numeric},2,3]=tensor([[1,2,3],[4,5,6]])
  b:Tensor[{numeric},3,2]=tensor([[7,8],[9,10],[11,12]])
  reduceSum(matmul(a,b))
''', '415'),
        ('map', f'''fn main()->{numeric}:
  a:Tensor[{numeric},3]=tensor([1,2,3])
  reduceSum(a*3+1)
''', '21'),
        ('empty-map', f'''fn main()->{numeric}:
  a:Tensor[{numeric},0]=tensor([])
  reduceSum(a*3+1)
''', '0'),
        ('packed', f'''fn ones(n:int)->[{numeric}]:
  if n == 0:[1]
  else:
    part=ones(n-1)
    concat(part,part)
fn main()->{numeric}:
  a:Tensor[{numeric},32,128]=tensor(ones(12),[32,128])
  b:Tensor[{numeric},128,32]=reshape(a,[128,32])
  reduceSum(matmul(a,b))
''', '131072'),
    ]
    for name, source, expected in cases:
        path = a.output / (numeric + '-' + name + '.k')
        path.write_text(source)
        for variant in variants:
            binary = a.output / (numeric + '-' + name + '-' + variant.lstrip('-'))
            run([a.compiler, path, variant, '-o', binary])
            assert run([binary]) == expected, (numeric, name, variant)
            print(numeric, name, variant, 'passed', flush=True)
