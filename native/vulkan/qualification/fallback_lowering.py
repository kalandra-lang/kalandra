"""Reject accidental lowering of supported float32 in the float64 fallback pass."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser()
p.add_argument('--lower',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
source=a.output/'mixed.mlir'
source.write_text('''module {
  func.func @mixed(%a: f32, %b: f32, %x: f64, %y: f64) -> (f32, i1, f64) {
    %sum = arith.addf %a, %b : f32
    %test = arith.cmpf olt, %a, %b : f32
    %wide = arith.mulf %x, %y : f64
    return %sum, %test, %wide : f32, i1, f64
  }
}
''')
output=a.output/'mixed.lowered.mlir'
result=subprocess.run([a.lower,source,output,'hardware-with-software-fallback'],capture_output=True,text=True)
assert not result.returncode,result.stderr
text=output.read_text()
assert 'arith.addf' in text and 'arith.cmpf' in text,text
assert 'arith.mulf' not in text and 'arith.muli' in text,text
# The ordinary software pass remains fail-closed for an unsupported comparison;
# the new mode must not silently change existing Metal/software behavior.
result=subprocess.run([a.lower,source,a.output/'software.lowered.mlir','software'],capture_output=True,text=True)
assert result.returncode and 'strict binary32' in result.stderr,result
source=a.output/'unsupported.mlir'
source.write_text('''module {
  func.func @unsupported(%a: f16, %b: f16) -> f16 {
    %sum = arith.addf %a, %b : f16
    return %sum : f16
  }
}
''')
result=subprocess.run([a.lower,source,a.output/'unsupported.lowered.mlir','hardware-with-software-fallback'],capture_output=True,text=True)
assert result.returncode and 'strict binary32' in result.stderr,result
print('PASS: selective float64 fallback retains native float32/comparisons; unsupported types fail closed',flush=True)
