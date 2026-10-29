"""Hardware float32 DAG work proofs grow with expressions; software stays unproved."""
import argparse, re, subprocess
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
costs = []
for arithmetic in ('hardware', 'software'):
    for terms in (1, 17):
        source = args.output / f'{arithmetic}-{terms}.k'
        expression = 'values*scale' + '+scale'*terms
        source.write_text(f"""fn operation(values:Tensor[float32,?,?],scale:float32)->Tensor[float32,?,?]:
  transpose(transpose({expression}))
fn main()->Tensor[float32,?,?] ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float32,1,2]=tensor([[1,2]])
    resident=try await transfer(values,gpu)
    answer=try await spawn operation(resident,0.25) on gpu
    try await transfer(answer,devices.cpu())
""")
        bundle = source.with_suffix('')
        built = subprocess.run([str(args.compiler.resolve()), str(source), '--target', 'metal',
                                '--arithmetic', arithmetic, '-O2', '-o', str(bundle)],
                               text=True, capture_output=True, timeout=180)
        assert built.returncode == 0, built.stderr
        host = (bundle/'host.cpp').read_text()
        maps = re.findall(r'NumericInstructionKind::Map,\*kernel\d+,operands\d+,scalars\d+,\d+u,([01])u(?:,(\d+)u)?\}', host)
        assert len(maps) >= 3, maps
        if arithmetic == 'software':
            assert all(not cost for _, cost in maps), maps
        maps = [(flag, cost) for flag, cost in maps if flag == '1']
        assert maps
        if arithmetic == 'hardware':
            assert all(cost and int(cost) >= 32 and int(cost) % 32 == 0 for _, cost in maps), maps
            costs.append(sum(int(cost) for _, cost in maps))
        else:
            assert all(not cost for _, cost in maps), maps
assert costs[1] > costs[0], costs
print(f'Hardware expression costs grow: {costs}; software arithmetic has no work proof.')
