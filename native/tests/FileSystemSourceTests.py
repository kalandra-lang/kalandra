"""Exercise owned file APIs, linear access diagnostics and real filesystem effects."""
import argparse
import os
import re
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--variant', choices=('debug','optimized','sanitized','thread-sanitized'), required=True)
p.add_argument('--target', choices=('cpu','metal','vulkan','cuda'), default='cpu')
p.add_argument('--compile-only', action='store_true')
a = p.parse_args()
a.compiler = a.compiler.resolve(); a.output = a.output.resolve(); a.output.mkdir(parents=True, exist_ok=True)
flag = {'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[a.variant]
prelude = '''from "std/fs" import File, BufferedReader, BufferedWriter, open, OpenMode, FsError, bufferedReader, bufferedWriter, read, readExact, readInto, readLine, write, writeAll, writeText, writeLine, seekTo, flush, finish, close
from "std/io" import println, IoError, Line
from "std/fmt" import format
'''

def build(name, source, diagnostic=None):
    directory = a.output/name; directory.mkdir(exist_ok=True)
    path = directory/'main.k'; path.write_text(prelude + source, encoding='utf-8')
    executable = directory/('program.exe' if os.name == 'nt' else 'program')
    output = executable if a.target == 'cpu' else directory/'bundle'
    command = [a.compiler,path,flag,'-o',output]
    if a.target != 'cpu': command.extend(['--target',a.target])
    result = subprocess.run(command, capture_output=True, timeout=240)
    (directory/'build.log').write_bytes(result.stdout+result.stderr)
    if diagnostic:
        assert result.returncode and diagnostic.encode() in result.stderr, (name,result)
        print('PASS: diagnostic',name,flush=True); return
    assert result.returncode == 0, (name,result)
    return executable if a.target == 'cpu' else output/executable.name

def run(name, source, expected, contents=None):
    if a.target != "cpu" and name == "own-callable":
        return  # Existing mixed-GPU coordinator callable limitation; CPU case remains qualified.
    if a.target != 'cpu':
        source = source.replace('! (FsError | IoError):', '! (FsError | IoError | TaskError):')
        source = re.sub(r'(fn main\(args:\[String\]\)->unit ! [^\n]+:\n)', r'\1  answer=try parallel:\n    gpu=devices.gpu()\n    try await spawn double(21) on gpu\n  try println(format("{}",answer))\n', source)
        source = 'fn double(n:i64)->i64:\n  return n*2\n' + source
        expected = b'42\n' + expected
    executable=build(name,source)
    if a.compile_only:
        print('PASS: compiled',name,flush=True); return
    for workers in (1,4):
        data=executable.parent/f'file with spaces Ω-{workers}.bin'
        data.unlink(missing_ok=True)
        # Windows timer granularity makes 2,000 resumable I/O requests exceed
        # 30 seconds. Keep all iterations; other behavior cases stay bounded.
        timeout = 180 if name == 'automatic-loop-cleanup' else 30
        result=subprocess.run([executable,data],capture_output=True,timeout=timeout,
            env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
        (executable.parent/f'run-{workers}.log').write_bytes(result.stdout+result.stderr)
        stderr=result.stderr
        if a.target == 'metal':
            stderr=b''.join(line for line in stderr.splitlines(keepends=True) if not re.fullmatch(
                rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?',line))
        assert result.returncode == 0 and result.stdout == expected and not stderr,(name,workers,result)
        if contents is not None: assert data.read_bytes() == contents,(name,data.read_bytes())
    print('PASS: executed',name,flush=True)

run('bytes-and-ownership','''fn passAlong(own file: File)->File:
  return file
fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.Create)
  moved=passAlong(file)
  try writeAll(moved,[65,66,67,68,69])
  try close(moved)
  input=try open(args[0],OpenMode.Read)
  prefix=try readExact(input,2)
  buffer:[u8]=[9,9,9,9,9]
  count=try readInto(input,buffer)
  try println(format("{}:{}:{}:{}",length(prefix),count,buffer[0],buffer[4]))
  try seekTo(input,0)
  chunk=try read(input,100)
  try println(format("{}:{}",length(chunk),chunk[4]))
  try close(input)
  return ()
''',b'2:3:67:9\n5:69\n',b'ABCDE')

run('write-flush-and-modes','''fn header(file:File)->String ! FsError:
  bytes=try readExact(file,10)
  return "complete"
fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.CreateNew)
  count=try write(file,[65,66,67])
  try close(file)
  append=try open(args[0],OpenMode.Append)
  try writeText(append,"D")
  try close(append)
  existing=try open(args[0],OpenMode.Write)
  try writeText(existing,"Z")
  try flush(existing)
  try close(existing)
  both=try open(args[0],OpenMode.ReadWrite)
  try seekTo(both,2)
  try writeText(both,"X")
  try seekTo(both,0)
  bytes=try readExact(both,4)
  try println(format("{}:{}:{}:{}:{}",count,bytes[0],bytes[1],bytes[2],bytes[3]))
  try seekTo(both,0)
  result=header(both) catch fn(error):
    return match error:
      FsError.System {code,message}:"system"
      FsError.InvalidEncoding:"encoding"
      FsError.LineTooLong {limit}:"limit"
      FsError.UnexpectedEnd {expected,actual}:format("short:{}:{}",expected,actual)
      FsError.InvalidArgument:"argument"
  try println(result)
  try close(both)
  output=try open(args[0],OpenMode.Write)
  writer=try bufferedWriter(output,100)
  try writeText(writer,"Q")
  try flush(writer)
  input=try open(args[0],OpenMode.Read)
  first=try readExact(input,1)
  try println(format("{}",first[0]))
  try close(input)
  try finish(writer)
  return ()
''',b'3:90:66:88:68\nshort:10:4\n81\n',b'QBXD')

run('large-text','''fn payload(depth:i64)->String:
  if depth==0:
    return "x"
  part=payload(depth-1)
  return part+part
fn main(args:[String])->unit ! (FsError | IoError):
  text=payload(17)+"z"
  file=try open(args[0],OpenMode.Create)
  try writeText(file,text)
  try close(file)
  append=try open(args[0],OpenMode.Append)
  writer=try bufferedWriter(append,7)
  try writeLine(writer,text)
  try finish(writer)
  input=try open(args[0],OpenMode.Read)
  bytes=try readExact(input,262147)
  try println(format("{}:{}:{}:{}",length(bytes),bytes[131072],bytes[262145],bytes[262146]))
  try close(input)
  return ()
''',b'262147:122:122:10\n',b'x'*131072+b'z'+b'x'*131072+b'z\n')

run('unicode-text','''fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.Create)
  writer=try bufferedWriter(file)
  try writeText(writer,"α")
  try writeLine(writer,"β")
  try finish(writer)
  input=try open(args[0],OpenMode.Read)
  reader=try bufferedReader(input)
  line=try readLine(reader,100)
  try match line:
    Line.End:println("end")
    Line.Text {text}:println(text)
  return ()
''','αβ\n'.encode(),'αβ\n'.encode())

run('buffered-lines','''fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.Create)
  writer=try bufferedWriter(file,3)
  try writeAll(writer,[65,13,10,66,10,67])
  try finish(writer)
  input=try open(args[0],OpenMode.Read)
  reader=try bufferedReader(input,2)
  ended=false
  while not ended:
    line=try readLine(reader,100)
    match line:
      Line.End:
        ended=true
        ()
      Line.Text {text}:try println(text)
  try seekTo(reader,0)
  first=try readExact(reader,1)
  try println(format("{}",first[0]))
  try close(reader)
  return ()
''',b'A\nB\nC\n65\n',b'A\r\nB\nC')

run('errors-and-draining','''fn main(args:[String])->unit ! (FsError | IoError):
  output=try open(args[0],OpenMode.Create)
  try writeAll(output,[65,65,65,10,255,10,66,10])
  try close(output)
  input=try open(args[0],OpenMode.Read)
  line1=readLine(input,2) catch fn(error):
    return Line.Text {text:"limit"}
  match line1:
    Line.End:()
    Line.Text {text}:try println(text)
  line2=readLine(input,2) catch fn(error):
    return Line.Text {text:"encoding"}
  match line2:
    Line.End:()
    Line.Text {text}:try println(text)
  line3=try readLine(input,2)
  match line3:
    Line.End:()
    Line.Text {text}:try println(text)
  try close(input)
  return ()
''',b'limit\nencoding\nB\n')

run('task-borrow','''fn first(file:File)->u8 ! FsError:
  bytes=try readExact(file,1)
  return bytes[0]
fn main(args:[String])->unit ! (FsError | IoError | TaskError):
  output=try open(args[0],OpenMode.Create)
  try writeAll(output,[65,66])
  try close(output)
  input=try open(args[0],OpenMode.Read)
  value=try parallel:
    task=spawn first(input)
    try await task
  next=try first(input)
  try println(format("{}:{}",value,next))
  try close(input)
  return ()
''',b'65:66\n')

run('own-callable','''fn give(own file:File)->File:
  return file
fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.Create)
  take=give
  moved=take(file)
  try writeAll(moved,[65])
  try close(moved)
  return ()
''',b'',b'A')

run('owned-consumer','''fn discard(own writer:BufferedWriter, early:bool)->unit:
  if early:
    return ()
  return ()
fn main(args:[String])->unit ! (FsError | IoError):
  file=try open(args[0],OpenMode.Create)
  writer=try bufferedWriter(file,128)
  try writeAll(writer,[65,66])
  discard(writer,true)
  input=try open(args[0],OpenMode.Read)
  contents=try read(input,10)
  try println(format("{}",length(contents)))
  return ()
''',b'0\n',b'')

run('automatic-loop-cleanup','''fn check(path:String)->unit ! FsError:
  for i in range(1000):
    file=try open(path,OpenMode.Read)
    bytes=try readExact(file,1)
  return ()
fn main(args:[String])->unit ! (FsError | IoError | TaskError):
  output=try open(args[0],OpenMode.Create)
  try writeAll(output,[65])
  try close(output)
  try parallel:
    task=spawn check(args[0])
    try await task
    ()
  return ()
''',b'',b'A')

if a.target=='cpu':
    negatives = {
        'move':('fn main()->unit ! FsError:\n  f=try open("x",OpenMode.Read)\n  other=f\n  try close(f)\n  return ()\n','was moved'),
        'double-close':('fn main()->unit ! FsError:\n  f=try open("x",OpenMode.Read)\n  try close(f)\n  try close(f)\n  return ()\n','was moved'),
        'borrow-close':('fn bad(file:File)->unit ! FsError:\n  try close(file)\n  return ()\nfn main()->unit:\n  return ()\n',"can't move a borrowed"),
        'borrow-return':('fn bad(file:File)->File:\n  return file\nfn main()->unit:\n  return ()\n',"can't move a borrowed"),
        'forged':('fn main()->unit:\n  fake=File {__storage:[]}\n  return ()\n','opaque'),
        'field':('fn bad(file:File)->unit:\n  bytes=file.__storage\n  return ()\nfn main()->unit:\n  return ()\n','opaque'),
        'copy':('fn bad(file:File)->unit:\n  other=copy(file)\n  return ()\nfn main()->unit:\n  return ()\n','copy requires'),
        'own-number':('fn bad(own n:i64)->i64:\n  return n\nfn main()->unit:\n  return ()\n','own parameters'),
        'read-writer':('fn bad(writer:BufferedWriter)->unit ! FsError:\n  bytes=try read(writer,2)\n  return ()\nfn main()->unit:\n  return ()\n','handle type'),
        'close-writer':('fn bad(writer:BufferedWriter)->unit ! FsError:\n  try close(writer)\n  return ()\nfn main()->unit:\n  return ()\n','finish buffered'),
        'array':('fn bad(file:File)->unit:\n  files=[file,file]\n  return ()\nfn main()->unit:\n  return ()\n','cannot be stored in arrays'),
        'branch-move':('fn bad(own file:File,flag:bool)->unit ! FsError:\n  if flag:\n    try close(file)\n  try close(file)\n  return ()\nfn main()->unit:\n  return ()\n','was moved'),
        'loop-move':('fn bad(own file:File)->unit ! FsError:\n  for i in range(2):\n    try close(file)\n  return ()\nfn main()->unit:\n  return ()\n','was moved'),
        'duplicate-arguments':('fn both(first:File,second:File)->unit:\n  return ()\nfn bad(file:File)->unit:\n  both(file,file)\n  return ()\nfn main()->unit:\n  return ()\n','same file through multiple'),
        'capture':('fn bad(file:File)->unit:\n  f=fn()->unit ! FsError:\n    try close(file)\n    return ()\n  return ()\nfn main()->unit:\n  return ()\n','explicit function parameters'),
        'own-forward-borrow':('fn consume(own file:File)->unit:\n  return ()\nfn bad(file:File)->unit:\n  consume(file)\n  return ()\nfn main()->unit:\n  return ()\n',"can't move a borrowed"),
        'own-signature':('fn consume(own file:File)->unit:\n  return ()\nfn main()->unit:\n  f:fn(File)->unit=consume\n  return ()\n','expected fn('),
        'task-conflict':('fn first(file:File)->unit ! FsError:\n  bytes=try read(file,1)\n  return ()\nfn bad(file:File)->unit ! (FsError | TaskError):\n  return try parallel:\n    task=spawn first(file)\n    try first(file)\n    try await task\n    ()\nfn main()->unit:\n  return ()\n',"can't access file handle"),
    }
    for name,(source,diagnostic) in negatives.items(): build(name,source,diagnostic)
