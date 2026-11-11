"""Owned byte buffers, filesystem overloads, access rules and mixed GPU hosts."""
import argparse,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
p.add_argument('--target',choices=('cpu','metal','vulkan','cuda'),default='cpu')
p.add_argument('--compile-only',action='store_true')
a=p.parse_args();a.compiler=a.compiler.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
flag={'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[a.variant]
prelude='''from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append, reserve, truncate, clear, toArray, intoArray, capacity, length
from "std/fs" import open, OpenMode, FsError, readInto, readAppend, write, writeAll, close, bufferedReader, bufferedWriter, finish, seekTo
from "std/io" import println, IoError
from "std/fmt" import format
from "std/time" import sleep, milliseconds, TimeError
'''
main='fn main()->unit ! (BufferError | FsError | IoError | TimeError | TaskError):\n'
def run(name,source,expected,fault=None):
    root=a.output/name;root.mkdir(exist_ok=True)
    if a.target!='cpu':
        source='fn twice(n:i64)->i64:\n  return n*2\n'+source
        source=source.replace(main,main+'  checked=try parallel:\n    gpu=devices.gpu()\n    try await spawn twice(21) on gpu\n  try println(format("{}",checked))\n')
        expected='42\n'+expected
    path=root/'main.k';path.write_text(prelude+source,encoding='utf-8')
    binary=root/('program.exe' if os.name=='nt' else 'program');output=binary if a.target=='cpu' else root/'bundle'
    cmd=[a.compiler,path,flag,'-o',output]
    if a.target!='cpu':cmd+=['--target',a.target]
    r=subprocess.run(cmd,capture_output=True,timeout=240);(root/'build.log').write_bytes(r.stdout+r.stderr)
    assert r.returncode==0,(name,r.stderr.decode(errors='replace'))
    if a.compile_only:print('PASS: compiled',name,flush=True);return
    if a.target!='cpu':binary=output/binary.name
    for workers in ('1','4'):
        r=subprocess.run([binary],cwd=root,capture_output=True,env=dict(os.environ,KALANDRA_WORKERS=workers),timeout=120)
        (root/f'run-{workers}.log').write_bytes(r.stdout+r.stderr)
        stderr=r.stderr
        if a.target=='metal':stderr=b''.join(x for x in stderr.splitlines(keepends=True) if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?',x))
        if fault:
            # Mixed GPU supervisors report the typed runtime fault as task
            # failure kind/code (Fault=1, BoundsFault=5), rather than CPU CLI text.
            diagnosed = fault.encode() in stderr or (fault == 'BoundsFault' and a.target != 'cpu' and
                        re.search(rb'task failed: 1/5(?:\s|$)', stderr) is not None)
            assert r.returncode>0 and r.stdout==expected.encode() and diagnosed,(name,workers,r.returncode,r.stdout,stderr)
            assert not any(x in stderr for x in (b'AddressSanitizer',b'ThreadSanitizer',b'runtime error:')),(name,stderr)
        else:
            assert r.returncode==0 and r.stdout==expected.encode() and not stderr,(name,workers,r.returncode,r.stdout,stderr)
    print('PASS: executed',name,flush=True)
run('operations',main+'''  b=try byteBuffer(2)
  try append(b,[1,2,3])
  snapshot=toArray(b)
  b[0]=9
  try truncate(b,2)
  try reserve(b,32)
  enough=capacity(b)>=32
  clear(b)
  try append(b,[7,8])
  values=intoArray(b)
  try println(format("{}:{}:{}:{}:{}",snapshot[0],snapshot[2],values[0],length(values),enough))
  return ()
''','1:3:7:2:true\n')
run('filesystem',main+'''  b=try byteBuffer()
  try append(b,[65,66,0,68,69,70])
  output=try bufferedWriter(try open("bytes.bin",OpenMode.Create),2)
  try writeAll(output,b)
  try finish(output)
  input=try bufferedReader(try open("bytes.bin",OpenMode.Read),2)
  clear(b)
  while true:
    count=try readAppend(input,b,3)
    if count==0:
      break
  try println(format("{}:{}:{}:{}",length(b),b[0],b[2],b[5]))
  count=try readInto(input,b,8)
  try println(format("{}:{}",count,length(b)))
  try seekTo(input,0)
  values:[u8]=[9,9,9,9]
  amount=try readInto(input,values)
  try println(format("{}:{}:{}",amount,values[0],values[3]))
  try close(input)
  raw=try open("raw.bin",OpenMode.Create)
  try append(b,[1,2,3])
  written=try write(raw,b)
  try println(format("{}",written))
  try close(raw)
  rawInput=try open("raw.bin",OpenMode.Read)
  copied=try readInto(rawInput,b,2)
  try println(format("{}:{}:{}",copied,length(b),b[1]))
  try close(rawInput)
  return ()
''','6:65:0:70\n0:0\n2:65:9\n3\n2:2:2\n')
run('recursive-ownership','''fn carry(own b:ByteBuffer,n:i64)->ByteBuffer ! TimeError:
  if n==0:
    try sleep(try milliseconds(1))
    return b
  return try carry(b,n-1)
'''+main+'''  b=try byteBuffer(32)
  try append(b,[42])
  returned=try carry(b,2000)
  values=intoArray(returned)
  try println(format("{}:{}",length(values),values[0]))
  return ()
''','1:42\n')
run('function-values',main+'''  make=byteBuffer
  add=append
  snapshot=toArray
  consume=intoArray
  b=try make(16)
  try add(b,[40,2])
  saved=snapshot(b)
  b[0]=41
  values=consume(b)
  try println(format("{}:{}:{}",saved[0],values[0],values[1]))
  return ()
''','40:41:2\n')
run('task-readers','''fn inspect(b:ByteBuffer)->i64:
  return length(b)+i64(b[0])
'''+main+'''  b=try byteBuffer()
  try append(b,[5,6])
  answer=try parallel:
    first=spawn inspect(b)
    second=spawn inspect(b)
    a=try await first
    c=try await second
    a+c
  try append(b,[7])
  try println(format("{}:{}",answer,length(b)))
  return ()
''','14:3\n')
run('owned-task','''fn build(own b:ByteBuffer)->[u8] ! BufferError:
  try append(b,[2])
  return intoArray(b)
'''+main+'''  b=try byteBuffer()
  try append(b,[40])
  values=try parallel:
    work=spawn build(b)
    try await work
  try println(format("{}:{}",values[0],values[1]))
  return ()
''','40:2\n')
run('repeat-and-errors','''fn invalidReserve(mut b:ByteBuffer)->i64 ! BufferError:
  try reserve(b,-1)
  return 0
fn invalidTruncate(mut b:ByteBuffer)->i64 ! BufferError:
  try truncate(b,1)
  return 0
'''+main+'''  total:i64=0
  for i in range(1000):
    b=try byteBuffer()
    try append(b,[40,2])
    values=intoArray(b)
    total=total+i64(values[0])+i64(values[1])
  b=try byteBuffer()
  invalid=invalidReserve(b) catch fn(error):
    return match error:
      BufferError.InvalidSize:11
      BufferError.OutOfRange:12
  outside=invalidTruncate(b) catch fn(error):
    return match error:
      BufferError.InvalidSize:21
      BufferError.OutOfRange:22
  try println(format("{}:{}:{}",total,invalid,outside))
  return ()
''','42000:11:22\n')
placement = ' on gpu' if a.target != 'cpu' else ''
run('converted-array-input','''fn sumBytes(values:[u8])->i64:
  return i64(values[0])+i64(values[1])
'''+main+'''  b=try byteBuffer()
  try append(b,[40,2])
  values=intoArray(b)
  answer=try parallel:
'''+('    gpu=devices.gpu()\n' if a.target!='cpu' else '')+f'''    task=spawn sumBytes(values){placement}
    try await task
  try println(format("{{}}",answer))
  return ()
''','42\n')
run('array-literal-io',main+'''  output=try open("literal.bin",OpenMode.Create)
  try writeAll(output,[42])
  try writeAll(output,[])
  try close(output)
  input=try open("literal.bin",OpenMode.Read)
  b=try byteBuffer()
  count=try readInto(input,b,1)
  try println(format("{}:{}",count,b[0]))
  try close(input)
  return ()
''','1:42\n')
run('readonly-aliases','''fn inspect(a:ByteBuffer,b:ByteBuffer)->i64:
  return length(a)+length(b)
'''+main+'''  b=try byteBuffer()
  try append(b,[1,2,3])
  try println(format("{}",inspect(b,b)))
  return ()
''','6\n')
run('spare-capacity',main+'''  b=try byteBuffer(16)
  try println(format("{}",b[0]))
  return ()
''','',fault='BoundsFault')
run('bounds-before-rhs','''fn index(b:ByteBuffer)->i64:
  return length(b)
fn rhs()->u8 ! IoError:
  try println("unexpected RHS")
  return 1
'''+main+'''  b=try byteBuffer(16)
  b[index(b)]=try rhs()
  return ()
''','',fault='BoundsFault')
run('bounds-after-rhs','''fn index(b:ByteBuffer)->i64:
  return length(b)-1
fn erase(mut b:ByteBuffer)->u8:
  clear(b)
  return 1
'''+main+'''  b=try byteBuffer(16)
  try append(b,[5])
  b[index(b)]=erase(b)
  return ()
''','',fault='BoundsFault')
run('bounds-high-u64','''fn index(b:ByteBuffer)->u64:
  return u64(capacity(b))+9223372036854775808
'''+main+'''  b=try byteBuffer()
  try append(b,[5])
  try println(format("{}",b[index(b)]))
  return ()
''','',fault='BoundsFault')
# These diagnostics are source contracts, checked identically for every backend.
negative={
 'use-after-move':(main+'  a=try byteBuffer()\n  b=a\n  try println(format("{}",length(a)))\n  return ()\n','S081'),
 'use-after-consume':(main+'  b=try byteBuffer()\n  values=intoArray(b)\n  try println(format("{}",length(b)))\n  return ()\n','S081'),
 'readonly-mutation':('fn change(b:ByteBuffer)->unit ! BufferError:\n  try append(b,[1])\n  return ()\n'+main+'  b=try byteBuffer()\n  try change(b)\n  return ()\n','S077'),
 'borrow-return':('fn take(b:ByteBuffer)->ByteBuffer:\n  return b\n'+main+'  b=try byteBuffer()\n  c=take(b)\n  return ()\n','S081'),
 'spawn-mut':('fn edit(mut b:ByteBuffer)->unit:\n  clear(b)\n  return ()\n'+main+'  b=try byteBuffer()\n  try parallel:\n    task=spawn edit(b)\n    try await task\n  return ()\n','S077'),
 'reader-conflict':('fn read(b:ByteBuffer)->i64:\n  return length(b)\n'+main+'  b=try byteBuffer()\n  try parallel:\n    task=spawn read(b)\n    try append(b,[1])\n    try await task\n  return ()\n','S077'),
 'alias-mut-first':('fn edit(mut a:ByteBuffer,b:ByteBuffer)->unit:\n  clear(a)\n  return ()\n'+main+'  b=try byteBuffer()\n  edit(b,b)\n  return ()\n','S081'),
 'alias-mut-last':('fn edit(a:ByteBuffer,mut b:ByteBuffer)->unit:\n  clear(b)\n  return ()\n'+main+'  b=try byteBuffer()\n  edit(b,b)\n  return ()\n','S081'),
 'missing-read-limit':(main+'  b=try byteBuffer()\n  file=try open("bytes.bin",OpenMode.Read)\n  count=try readInto(file,b)\n  return ()\n','S027'),
 'array-extra-read-limit':(main+'  b:[u8]=[0]\n  file=try open("bytes.bin",OpenMode.Read)\n  count=try readInto(file,b,1)\n  return ()\n','S081'),
}
for name,(source,code) in negative.items():
    root=a.output/name;root.mkdir(exist_ok=True);path=root/'main.k';path.write_text(prelude+source,encoding='utf-8')
    command=[a.compiler,path,'-O2','-o',root/'unexpected']
    if a.target!='cpu':command+=['--target',a.target]
    r=subprocess.run(command,capture_output=True,timeout=120);(root/'build.log').write_bytes(r.stdout+r.stderr)
    assert r.returncode==1 and ('error['+code+']').encode() in r.stderr,(name,r.returncode,r.stderr)
    print('PASS: diagnosed',name,flush=True)
