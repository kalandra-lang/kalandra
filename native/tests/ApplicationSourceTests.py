"""Qualify normal application entry behavior without numerical observation adapters."""
import argparse
import os
import re
from pathlib import Path
import subprocess

from ParallelTestRunner import run_parallel

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
a=p.parse_args();a.compiler=a.compiler.resolve();a.output.mkdir(parents=True,exist_ok=True)
flags={'debug':['-O0'],'optimized':['-O2'],'sanitized':['--sanitize'],'thread-sanitized':['--thread-sanitize']}[a.variant]
console='from "std/io" import println, IoError\nfrom "std/fmt" import format\n'

def build(name,source):
    directory=a.output/name;directory.mkdir(exist_ok=True)
    path=directory/'main.k';path.write_text(source, encoding='utf-8')
    exe=directory/('program.exe' if os.name=='nt' else 'program')
    result=subprocess.run(list(map(str,[a.compiler,path,*flags,'-o',exe])),capture_output=True,timeout=180)
    (directory/'build.log').write_bytes(result.stdout+result.stderr)
    assert result.returncode==0,(name,result)
    return exe

def run(exe,*args,status=0,out=b'',err=b''):
    result=subprocess.run([exe,*args],capture_output=True,timeout=15)
    assert result.returncode==status and result.stdout==out and result.stderr==err,(exe,result,status,out,err)
    print('PASS:',exe.parent.name,flush=True)

SOURCES = {
    'unit': 'fn main()->unit:\n  return ()\n',
    **{'exit-'+str(code): 'from "std/process" import ExitStatus\nfn main()->ExitStatus:\n  return ExitStatus {code:'+str(code)+'}\n' for code in (0,1,42,255)},
    'arguments': console+'''from "std/process" import ExitStatus
fn main(args:[String])->ExitStatus ! IoError:
  try println(format("{}:{}:{}:{}",length(args),args[0],args[1],args[2]))
  return ExitStatus {code:42}
''',
    'empty-arguments': console+'''fn main(args:[String])->unit ! IoError:
  try println(format("{}",length(args)))
  return ()
''',
    'task-arguments': console+'''fn count(args:[String])->i64:
  return length(args)
fn main(args:[String])->unit ! (IoError | TaskError):
  size=try parallel:
    task=spawn count(args)
    try await task
  try println(format("{}",size))
  return ()
''',
    'scoped-loop': console+'''from "std/io" import readLine, Line
fn main()->unit ! (IoError | TaskError):
  return try parallel:
    for i in range(1000):
      line=try readLine(2048)
      try match line:
        Line.End:fail IoError.InvalidEncoding
        Line.Text {text}:
          if byteLength(text)!=1024:fail IoError.InvalidEncoding
          ()
    ()
''',
    'sequential-config': console+'''fn main()->unit ! IoError:
  try println(format("value={}",42))
  return ()
''',
    'scheduled-config': console+'''fn main()->unit ! (IoError | TaskError):
  return try parallel:
    try println("never")
    ()
''',
    'sequential-line': console+'''from "std/io" import readLine, Line
fn main()->unit ! IoError:
  line=try readLine(2000000)
  try match line:
    Line.End:fail IoError.InvalidEncoding
    Line.Text {text}:
      if byteLength(text)!=1500000:fail IoError.InvalidEncoding
      ()
  return ()
''',
    'task-line-budget': console+'''from "std/io" import readLine
fn main()->unit ! (IoError | TaskError):
  return try parallel:
    line=try readLine(2000000)
    ()
''',
    'unhandled': 'error E:\n  Bad\nfn main()->unit ! E:\n  fail E.Bad\n',
    'sequential-tensor-cleanup': console+'''fn main()->unit ! IoError:
  values:Tensor[i32,2]=tensor([20,22])
  converted=f64(values)
  try println(format("{}",i64(reduceSum(converted))))
  return ()
''',
    'fault': 'fn divide(value:i64)->i64:\n  return 1/value\nfn main()->unit:\n  value=divide(0)\n  return ()\n',
    'argument-failures': 'fn main(args:[String])->unit:\n  return ()\n',
}

exes = dict(run_parallel(SOURCES.items(), lambda item: (item[0], build(item[0], item[1]))))

run(exes['unit'])
for code in (0,1,42,255):
    run(exes['exit-'+str(code)],status=code)
run(exes['arguments'],'é👩‍💻','hello world','',status=42,out='3:é👩‍💻:hello world:\n'.encode())
run(exes['empty-arguments'],out=b'0\n')
run(exes['task-arguments'],'α','β',out=b'2\n')

result=subprocess.run([exes['scoped-loop']],input=(b'x'*1024+b'\n')*1000,capture_output=True,timeout=60,
    env=dict(os.environ,KALANDRA_WORKERS='1'))
assert result.returncode==0 and not result.stdout and not result.stderr,result
print('PASS: wrapped application loops retire input under the existing task resource budget',flush=True)

result=subprocess.run([exes['sequential-config']],capture_output=True,timeout=15,env=dict(os.environ,KALANDRA_WORKERS='unused'))
assert result.returncode==0 and result.stdout==b'value=42\n' and not result.stderr,result
result=subprocess.run([exes['scheduled-config']],capture_output=True,timeout=15,env=dict(os.environ,KALANDRA_WORKERS='unused'))
assert result.returncode==1 and not result.stdout and b'AdmissionRejected' in result.stderr,result
print('PASS: proven sequential I/O omits scheduling; structured tasks retain admission checks',flush=True)

result=subprocess.run([exes['sequential-line']],input=b'x'*1500000+b'\n',capture_output=True,timeout=30)
assert result.returncode==0 and not result.stdout and not result.stderr,result
result=subprocess.run([exes['task-line-budget']],input=b'x'*1500000+b'\n',capture_output=True,timeout=30)
assert result.returncode==1 and not result.stdout and b'AllocationFault' in result.stderr,result
print('PASS: sequential line bound is explicit; actual tasks retain the existing memory budget',flush=True)

run(exes['sequential-tensor-cleanup'],out=b'42\n')
result=subprocess.run([exes['unhandled']],capture_output=True,timeout=15)
assert result.returncode==1 and not result.stdout and result.stderr.replace(b'\r\n',b'\n')==b'E.Bad\n',result
result=subprocess.run([exes['fault']],capture_output=True,timeout=15)
assert result.returncode==1 and not result.stdout and b'ArithmeticFault' in result.stderr,result
print('PASS: unhandled errors and runtime faults use stderr and status one',flush=True)

args_exe=exes['argument-failures']
successes=failures=0
for ordinal in range(1,25):
    result=subprocess.run([args_exe,'é','hello',''],capture_output=True,timeout=15,env=dict(os.environ,
        KALANDRA_TEST_FAIL_ALLOC_AT=str(ordinal),KALANDRA_TEST_REPORT_CLEANUP='1'))
    if result.returncode:
        failures+=1
        match=re.search(rb'cleanup: live=0 allocations=(\d+) releases=(\d+)',result.stderr)
        assert result.returncode==1 and b'AllocationFault' in result.stderr and match and match[1]==match[2] and not result.stdout,result
    else:
        successes+=1
        assert not result.stdout and not result.stderr,result
assert successes and failures
print('PASS: arguments allocate owned text and clean every injected failure',flush=True)
if os.name!='nt':
    result=subprocess.run([os.fsencode(args_exe),b'\xff'],capture_output=True,timeout=15)
    assert result.returncode==1 and not result.stdout and b'valid Unicode' in result.stderr,result
    print('PASS: malformed command-line text rejected before source main',flush=True)
if a.variant=='debug':
    for name,source in {
        'numeric':'fn main()->i64:\n  return 42\n',
        'fake-exit':'type ExitStatus:\n  code:u8\nfn main()->ExitStatus:\n  return ExitStatus {code:0}\n',
        'wrong-args':'fn main(args:[i64])->unit:\n  return ()\n',
    }.items():
        directory=a.output/('reject-'+name);directory.mkdir(exist_ok=True)
        path=directory/'main.k';path.write_text(source, encoding='utf-8')
        exe=directory/('program.exe' if os.name=='nt' else 'program');exe.write_bytes(b'preserved')
        result=subprocess.run(list(map(str,[a.compiler,path,'-o',exe])),capture_output=True,timeout=30)
        assert result.returncode and b'[S081]' in result.stderr and exe.read_bytes()==b'preserved',result
    directory=a.output/'observer';directory.mkdir(exist_ok=True)
    path=directory/'main.k';path.write_text('fn main()->i64:\n  return 42\n', encoding='utf-8')
    exe=directory/('program.exe' if os.name=='nt' else 'program')
    result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','main','-o',exe])),capture_output=True,timeout=180)
    assert result.returncode==0,result
    result=subprocess.run([exe],capture_output=True,timeout=15)
    assert result.returncode==0 and not result.stderr and result.stdout.replace(b'\r\n',b'\n')==b'42\n',result
    print('PASS: entry diagnostics preserve outputs; numerical test entry remains explicit',flush=True)
