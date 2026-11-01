"""Generate private typed arithmetic artifacts; no GPU execution or timing."""
import argparse
import pathlib
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=pathlib.Path, required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
fixture = pathlib.Path(__file__).parents[1].joinpath('tests/elementwise.mlir').read_text()
for chip in ('sm_80', 'sm_120'):
    for element in ('f32', 'f64', 'i8', 'i16', 'i32', 'i64'):
        source = fixture.replace('f32', element)
        if element.startswith('i'):
            source = source.replace('arith.mulf', 'arith.muli').replace('arith.addf', 'arith.addi')
        path = a.output / (element + '.mlir')
        path.write_text(source)
        for policy in ('hardware', 'software', 'hardware-with-software-fallback'):
            subprocess.run([str(a.compiler.resolve()), str(path), str(a.output / f'{chip}-{element}-{policy}.ptx'), policy, chip], check=True, capture_output=True)
source = pathlib.Path(__file__).parents[1].joinpath('tests/index-width.mlir')
subprocess.run([str(a.compiler.resolve()), str(source), str(a.output / 'index-width.ptx'), 'hardware', 'sm_80'], check=True, capture_output=True)
print('Generated 36 typed PTX modules (integer instructions serve signed and unsigned storage)')
