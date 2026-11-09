"""Run standard console I/O through source checking, task lowering and real streams."""
import argparse
import os
from pathlib import Path
import subprocess
import threading
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
a=p.parse_args();a.compiler=a.compiler.resolve();a.output.mkdir(parents=True,exist_ok=True)
flags={'debug':['-O0'],'optimized':['-O2'],'sanitized':['--sanitize'],'thread-sanitized':['--thread-sanitize']}[a.variant]
prelude='from "std/io" import print, println, eprint, eprintln, readLine, Line, IoError\nfrom "std/fmt" import format\n'
def build(name,source):
    directory=a.output/name;directory.mkdir(exist_ok=True)
    path=directory/'main.k';path.write_text(prelude+source)
    exe=directory/('program.exe' if os.name=='nt' else 'program')
    result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','main',*flags,'-o',exe])),capture_output=True,timeout=180)
    (directory/'build.log').write_bytes(result.stdout+result.stderr)
    assert result.returncode==0,(name,result)
    return exe

def run(exe,data=b'',out=b'',err=b''):
    for workers in (1,4):
        result=subprocess.run([exe],input=data,capture_output=True,timeout=15,env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
        assert result.returncode==0 and result.stdout==out and result.stderr==err,(exe,workers,result,out,err)
    print('PASS:',exe.parent.name,flush=True)

run(build('streams','''fn main()->unit ! IoError:
  try print("α\\0")
  try println(format("value={}",42))
  try eprint("error ")
  try eprintln("stream")
  return ()
'''),out='α\0value=42\n'.encode(),err=b'error stream\n')
read='''fn next()->String ! IoError:
  line=try readLine(100)
  return match line:
    Line.End:"<end>"
    Line.Text {text}:text
fn main()->unit ! IoError:
  try println(try next())
  try println(try next())
  try println(try next())
  return ()
'''
run(build('lines',read),'α\0é\r\nlast'.encode(),'α\0é\nlast\n<end>\n'.encode())
run(build('caught-errors','''fn next()->String ! IoError:
  line=try readLine(2)
  return match line:
    Line.End:"<end>"
    Line.Text {text}:text
fn recover()->String:
  return next() catch fn(error):
    return match error:
      IoError.System {code,message}:format("system={}:{}",code,message)
      IoError.InvalidEncoding:"encoding"
      IoError.LineTooLong {limit}:format("limit={}",limit)
fn main()->unit ! IoError:
  try println(recover())
  try println(recover())
  try println(recover())
  return ()
'''),b'long\n\xff\nok\n',b'limit=2\nencoding\nok\n')
run(build('closure','''fn main()->unit ! IoError:
  prefix="value="
  emit=fn(value:i64)->unit ! IoError:
    try println(format("{}{}",prefix,value))
    return ()
  try emit(42)
  return ()
'''),out=b'value=42\n')
run(build('task-output','''fn emit(value:i64)->unit ! IoError:
  try println(format("value={}",value))
  return ()
fn main()->unit ! (IoError | TaskError):
  return try parallel:
    first=spawn emit(42)
    try await first
    ()
'''),out=b'value=42\n')
run(build('loop-scopes','''fn main()->unit ! IoError:
  for i in range(3):
    line=try readLine(300000)
    match line:
      Line.End:()
      Line.Text {text}:try println(format("{}:{}",i,byteLength(text)))
  return ()
'''),b'a'*100000+b'\nbb\nccc\n',b'0:100000\n1:2\n2:3\n')

# The source bound is not an exemption from the scheduler's resource budget.
large=build('resource-bound','''fn main()->unit ! IoError:
  line=try readLine(2000000)
  return ()
''')
result=subprocess.run([large],input=b'a'*2000000+b'\n',capture_output=True,timeout=15)
assert result.returncode==86 and not result.stdout and b'AllocationFault' in result.stderr,result
print('PASS: oversized allocation preserves the task resource bound',flush=True)

# Keep stdin open with no data: EOF would conceal a worker-starvation bug.
exe=build('blocked-sibling','''fn input()->unit ! IoError:
  try eprintln("reading")
  line=try readLine(100)
  return ()
fn fault(divisor:i64)->i64:
  return 100/divisor
fn run()->i64 ! (IoError | TaskError):
  return try parallel:
    blocked=spawn input()
    failure=spawn fault(0)
    try await blocked
    try await failure
fn main()->i64:
  return run() catch fn(error):return 42
''')
for workers in (1,4):
    child=subprocess.Popen([exe],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                            env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
    timer=threading.Timer(10,child.kill);timer.start()
    try:
        status=child.wait(timeout=12) # Deliberately do not close stdin before task cancellation.
        output,error=child.communicate(timeout=2)
        assert status==0 and output.replace(b'\r\n',b'\n')==b'42\n' and b'reading\n'.startswith(error),(workers,status,output,error)
    finally:
        timer.cancel()
        if child.poll() is None:child.kill();child.communicate()
print('PASS: blocked input cancels after sibling fault with one and four workers',flush=True)
