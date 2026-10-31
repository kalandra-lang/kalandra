"""Inspect destination ABI selection and code generation; not execution evidence."""
import argparse
import json
from pathlib import Path
import re
import subprocess

SOURCES = {
    'calls': '''fn count(n:int,a:int)->int:
  if n==0:a
  else:count(n-1,a+1)
fn fib(n:int)->int:
  if n<2:n
  else:fib(n-1)+fib(n-2)
fn main()->int:
  count(1000,0)+fib(12)
''',
    'captures': '''fn make(offset:int)->fn(int)->int:
  fn(value:int):value+offset
fn walk(n:int,a:int,f:fn(int)->int)->int:
  if n==0:a
  else:walk(n-1,f(a),f)
fn main()->int:
  walk(1000,0,make(3))
''',
    'tasks': '''fn double(value:int)->int:
  value*2
fn main()->[int] ! TaskError:
  try parallelMap([1,2,3],double)
''',
    'records': '''type Cell:
  value:int
  flag:Bool
fn main()->int:
  cells=[Cell {value:3,flag:true},Cell {value:4,flag:false}]
  cells[0].value+cells[1].value
''',
    'floating': '''fn series(n:int)->float64:
  if n==0:1.0
  else:series(n-1)*1.000000001+0.25
fn main()->float64:
  series(100)
''',
}
TARGETS = ['x86_64-unknown-linux-gnu', 'aarch64-unknown-linux-gnu',
           'x86_64-pc-windows-msvc', 'aarch64-pc-windows-msvc', 'arm64-apple-macosx26.0.0']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--driver', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    def run(command):
        r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, (command, r.returncode, r.stdout[-1000:], r.stderr[-4000:])
        return r.stdout
    query = a.output / 'query.c'
    query.write_text('int query;\n')
    records = []
    for target in TARGETS:
        root = a.output / target
        root.mkdir(exist_ok=True)
        query_ir = root / 'query.ll'
        run([a.clang, '--no-default-config', '--target=' + target, '-S', '-emit-llvm', query, '-o', query_ir])
        layout = re.search(r'target datalayout = "([^"]+)"', query_ir.read_text())[1]
        layout_file = root / 'layout.txt'
        layout_file.write_text(layout)
        for name, text in SOURCES.items():
            source, ir = root / (name + '.k'), root / (name + '.ll')
            source.write_text(text)
            run([a.driver, source, target, layout_file, ir])
            contents = ir.read_text()
            assert f'target datalayout = "{layout}"' in contents
            assert f'target triple = "{target}"' in contents
            registers = re.findall(r'!\{!"(sp|rsp)"\}', contents)
            assert all(r == ('rsp' if target.startswith('x86_64') else 'sp') for r in registers)
            for mode in ('-O0', '-O2'):
                obj = root / (name + mode + '.obj')
                run([a.clang, '--no-default-config', '--target=' + target, '-Wno-override-module',
                     '-ffp-contract=off', mode, '-c', ir, '-o', obj])
                data = obj.read_bytes()
                if 'linux' in target:
                    assert data[:6] == b'\x7fELF\x02\x01'
                    assert int.from_bytes(data[18:20], 'little') == (62 if target.startswith('x86') else 183)
                elif 'windows' in target:
                    assert int.from_bytes(data[:2], 'little') == (0x8664 if target.startswith('x86') else 0xaa64)
                else:
                    assert data[:4] == b'\xcf\xfa\xed\xfe'
                    assert int.from_bytes(data[4:8], 'little') == 0x100000c
                records.append(dict(target=target, source=name, mode=mode, bytes=len(data)))
    # ABI rejection must be recoverable and must not publish an output file.
    for target, layout in [('armv7-unknown-linux-gnueabihf', 'e-p:32:32'),
                            ('aarch64-unknown-linux-gnu', 'E-p:64:64'),
                            ('aarch64-unknown-linux-gnu', 'invalid')]:
        layout_file = a.output / 'invalid-layout.txt'
        layout_file.write_text(layout)
        output = a.output / 'invalid.ll'
        r = subprocess.run(list(map(str, [a.driver, source, target, layout_file, output])),
                           capture_output=True, text=True, timeout=30)
        assert r.returncode == 1 and not output.exists(), (r.returncode, r.stderr)
    (a.output / 'qualification.json').write_text(json.dumps(dict(
        scope=__doc__, records=records), indent=2) + '\n')
    print(f'{len(records)} cross-target object checks passed; target execution is qualified separately')


if __name__ == '__main__':
    main()
