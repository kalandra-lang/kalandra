"""Deep execution and resource safety for the internal call convention."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--clang', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = a.output / 'calls.k'
source.write_text('''fn count(n:int,a:int)->int:
  if n==0:a
  else:count(n-1,a+1)
fn even(n:int)->Bool:
  if n==0:true
  else:odd(n-1,false)
fn odd(n:int,unused:Bool)->Bool:
  if n==0:false
  else:even(n-1)
fn jump(n:int,a:int)->int:
  if n==0:a
  else:
    next=if n%2==0:jump
    else:other
    next(n-1,a+1)
fn other(n:int,a:int)->int:
  if n==0:a
  else:
    next=if n%2==0:other
    else:jump
    next(n-1,a+1)
fn walk(n:int,a:int,f:fn(int)->int)->int:
  if n==0:a
  else:walk(n-1,f(a),f)
fn make(offset:int)->fn(int)->int:
  fn(value:int):value+offset
fn series(n:int)->float64:
  if n==0:1.0
  else:series(n-1)*1.000000001+0.25
fn faultTail(n:int)->int:
  if n==0:1/(n-n)
  else:faultTail(n-1)
fn faultDeep(n:int)->int:
  if n==0:1/(n-n)
  else:faultDeep(n-1)+1
type Pair:
  left:int
  right:int
  pad0:int
  pad1:int
  pad2:int
  pad3:int
  pad4:int
  pad5:int
fn recordTail(n:int,a:int)->Pair:
  if n==0:Pair {left:a,right:42,pad0:0,pad1:1,pad2:2,pad3:3,pad4:4,pad5:5}
  else:recordTail(n-1,a+1)
fn pair(n:int)->Pair:
  recordTail(n,0)
fn wide(n:int,a:int,b:int,c:int,d:int,e:int,f:int,g:int,h:int,i:int)->int:
  if n==0:a+b+c+d+e+f+g+h+i
  else:wide(n-1,b,c,d,e,f,g,h,i,a+1)
fn run(kind:int,n:int)->int:
  if kind==0:count(n,0)
  else:
    if kind==1:
      if even(n):1
      else:0
    else:
      if kind==2:jump(n,0)
      else:
        if kind==3:walk(n,0,make(3))
        else:
          if kind==4:int(series(n))
          else:
            if kind==5:faultTail(n)
            else:
              if kind==6:faultDeep(n)
              else:
                if kind==8:wide(n,0,1,2,3,4,5,6,7,8)
                else:
                  value=pair(n)
                  next=pair(n+1)
                  value.left+next.right
fn main()->int:
  run(0,1000000)
''')
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
compiler_sha = sha(a.compiler)
variants = [('debug', '-O0', ['-O0']), ('optimized', '-O2', ['-O2']),
            ('sanitized', '--sanitize', ['-O1', '-g', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'])]
if sys.platform != 'win32':
    variants.append(('thread-sanitized', '--thread-sanitize',
                     ['-O1', '-g', '-fsanitize=thread', '-fno-omit-frame-pointer']))
env = dict(os.environ)
if sys.platform == 'win32':
    resources = list(a.clang.resolve().parent.parent.glob('lib/clang/*/lib/windows'))
    env['PATH'] = os.pathsep.join(map(str, resources)) + os.pathsep + env.get('PATH', '')


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=240, env=env)
    assert result.returncode == 0, (command, result.returncode, result.stdout[-2000:],
                                    result.stderr[-4000:])
    return result.stdout


records = []
for name, flag, flags in variants:
    binary = a.output / (name + ('.exe' if sys.platform == 'win32' else ''))
    ir = binary.with_suffix('.ll')
    standalone = binary.with_name(binary.stem + '-standalone' + binary.suffix)
    run([a.compiler, source, '--test-entry', 'run', '--emit-llvm', ir, flag, '-o', standalone])
    text = ir.read_text()
    assert 'kalandra_recursive_enter_' not in text and 'tail.unwind' not in text
    runtime = a.compiler.parent / (('' if sys.platform == 'win32' else 'lib') +
        'kalandra-native-runtime' + ('-' + name if 'sanitized' in name else '') +
        ('.lib' if sys.platform == 'win32' else '.a'))
    run([a.clang, '-std=c++23', '-Wno-override-module', '-ffp-contract=off', '-pthread', *flags,
         *(['-fms-runtime-lib=static'] if sys.platform == 'win32' else []),
         root / 'native/tests/CallFoundationRuntimeTests.cpp', ir, runtime, '-o', binary])
    result = run([binary])
    assert run([standalone]) == '1000000\n'
    terminal = a.output / ('terminal-' + name + '.k')
    terminal.write_text('fn terminal(value:int)->int:\n  1/(value-value)\nfn main()->int:\n  terminal(0)\n')
    terminal_binary = binary.with_name('terminal-' + binary.name)
    run([a.compiler, terminal, flag, '-o', terminal_binary])
    stopped = subprocess.run([str(terminal_binary)], capture_output=True, text=True,
                             timeout=30, env=env)
    assert stopped.returncode == 86 and 'ArithmeticFault' in stopped.stderr and not stopped.stdout, (
        name, stopped.returncode, stopped.stdout, stopped.stderr)
    records.append(dict(variant=name, compiler_sha256=compiler_sha, ir_sha256=sha(ir),
                        binary_sha256=sha(binary), runtime_sha256=sha(runtime), result=result.strip()))
    print(name + ': ' + result.strip(), flush=True)
assert sha(a.compiler) == compiler_sha, 'compiler changed during qualification'
(a.output / 'qualification.json').write_text(json.dumps(records, indent=2) + '\n')
