"""Typed parsing through public source APIs, higher-order calls and GPU host coordination."""
import argparse
import os
from pathlib import Path
import re
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
p.add_argument('--target',choices=('cpu','metal','vulkan','cuda'),default='cpu')
p.add_argument('--compile-only',action='store_true')
a=p.parse_args();a.compiler=a.compiler.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
flag={'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[a.variant]
prelude='''from "std/parse" import parseI8, parseI16, parseI32, parseI64, parseU8, parseU16, parseU32, parseU64, parseF32, parseF64, parseBool, ParseError
from "std/io" import println, IoError
from "std/fmt" import format
'''
def build(name,source,diagnostic=None):
    root=a.output/name;root.mkdir(exist_ok=True);path=root/'main.k';path.write_text(prelude+source,encoding='utf-8')
    executable=root/('program.exe' if os.name=='nt' else 'program')
    output=executable if a.target=='cpu' else root/'bundle'
    command=[a.compiler,path,flag,'-o',output]
    if a.target!='cpu':command.extend(['--target',a.target])
    r=subprocess.run(command,capture_output=True,timeout=240)
    (root/'build.log').write_bytes(r.stdout+r.stderr)
    if diagnostic:
        assert r.returncode and diagnostic in r.stderr,(name,r)
        return None
    assert r.returncode==0,(name,r)
    return executable if a.target=='cpu' else output/executable.name

def run(name,source,expected):
    if a.target!='cpu':
        if name=='tasks-and-captures':
            source=source.replace('  parser=fn()->i64 ! ParseError:\n    return try parseI64(text)\n','').replace('spawn parser()','spawn parseWork(text)')
            source='fn parseWork(text:String)->i64 ! ParseError:\n  return try parseI64(text)\n'+source
        source=source.replace('! (ParseError | IoError):','! (ParseError | IoError | TaskError):')
        source=re.sub(r'(fn main\([^\n]+\n)',r'\1  answer=try parallel:\n    gpu=devices.gpu()\n    try await spawn double(21) on gpu\n  try println(format("{}",answer))\n',source)
        source='fn double(n:i64)->i64:\n  return n*2\n'+source
        expected='42\n'+expected
    executable=build(name,source)
    if a.compile_only:print('PASS: compiled',name,flush=True);return
    for workers in (1,4):
        r=subprocess.run([executable],capture_output=True,timeout=60,env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
        (executable.parent/f'run-{workers}.log').write_bytes(r.stdout+r.stderr)
        stderr=r.stderr
        if a.target=='metal':stderr=b''.join(x for x in stderr.splitlines(keepends=True) if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?',x))
        assert r.returncode==0 and r.stdout==expected.encode() and not stderr,(name,workers,r)
    print('PASS: executed',name,flush=True)

run('all-types','''fn main()->unit ! (ParseError | IoError):
  a:i8=try parseI8("-128")
  b:i16=try parseI16("-32768")
  c:i32=try parseI32("-2147483648")
  d:i64=try parseI64("-9223372036854775808")
  e:u8=try parseU8("255")
  f:u16=try parseU16("65535")
  g:u32=try parseU32("4294967295")
  h:u64=try parseU64("18446744073709551615")
  i:f32=try parseF32("0.125")
  j:f64=try parseF64("1.25e2")
  k:bool=try parseBool("true")
  try println(format("{}:{}:{}:{}",a,b,c,d))
  try println(format("{}:{}:{}:{}",e,f,g,h))
  try println(format("{}:{}:{}",i,j,k))
  return ()
''','-128:-32768:-2147483648:-9223372036854775808\n255:65535:4294967295:18446744073709551615\n0.125:125:true\n')

run('typed-errors','''fn label(text:String)->String ! ParseError:
  value=try parseU8(text)
  return format("byte={}",value)
fn main()->unit ! (ParseError | IoError):
  for text in ["12x","256","","-0","12"]:
    message=label(text) catch fn(error):
      return match error:
        ParseError.Invalid {offset}:format("invalid:{}",offset)
        ParseError.OutOfRange:"range"
    try println(message)
  return ()
''','invalid:2\nrange\ninvalid:0\ninvalid:0\nbyte=12\n')

run('float-rounding','''fn main()->unit ! (ParseError | IoError):
  a=try parseF32("16777217")
  b=try parseF64("9007199254740993")
  c=try parseF32("-0e99999999999999")
  d=try parseF64("inf")
  e=try parseF32("-inf")
  f=try parseF64("nan")
  try println(format("{}:{}",a==16777216,b==9007199254740992))
  try println(format("{}:{}:{}:{}",c,d,e,f))
  return ()
''','true:true\n-0:inf:-inf:nan\n')

run('function-values','''fn parseNumber(n:i64)->i64:
  return parseI64(format("{}",n)) catch fn(error):
    return -1
fn run(parser:fn(String)->i64 ! ParseError,text:String)->i64 ! ParseError:
  return try parser(text)
fn check()->i64 ! ParseError:
  parser=parseI64
  a=try run(parser,"42")
  values=map([1,2,3],parseNumber)
  return a*100+values[0]*10+values[2]
fn main()->unit ! (ParseError | IoError):
  value=try check()
  try println(format("{}",value))
  return ()
''','4213\n')

run('tasks-and-captures','''fn main()->unit ! (ParseError | IoError | TaskError):
  text="123"
  parser=fn()->i64 ! ParseError:
    return try parseI64(text)
  result=try parallel:
    work=spawn parser()
    try await work
  try println(format("{}",result))
  return ()
''','123\n')

run('long-text','''fn zeros(n:i64)->String:
  if n==0:
    return "0"
  half=zeros(n-1)
  return half+half
fn main()->unit ! (ParseError | IoError):
  text="0."+zeros(17)+"1e131073"
  value=try parseF64(text)
  try println(format("{}",value))
  return ()
''','1\n')

if a.target!='cpu':
    run('parsed-device-input','''fn scale(n:i64)->i64:
  return n*2
fn main()->unit ! (ParseError | IoError | TaskError):
  value=try parseI64("21")
  result=try parallel:
    gpu=devices.gpu()
    try await spawn scale(value) on gpu
  try println(format("{}",result))
  return ()
''','42\n')

if a.target=='cpu':
    for name,source,diagnostic in (
        ('wrong-input','fn main()->i64 ! ParseError:\n  return try parseI64(12)\n','S013'),
        ('missing-error','fn main()->i64:\n  return parseI64("12")\n','S036'),
        ('wrong-result','fn main()->u8 ! ParseError:\n  return try parseI64("12")\n','S010'),
        ('private-intrinsic','fn main()->i64:\n  return __kalandra_parse_i64("12")\n','S031')):
        root=a.output/name;root.mkdir(exist_ok=True);path=root/'main.k';path.write_text(prelude+source)
        r=subprocess.run([a.compiler,path,'-o',root/'bad'],capture_output=True,timeout=120)
        (root/'build.log').write_bytes(r.stdout+r.stderr)
        assert r.returncode and diagnostic.encode() in r.stderr,(name,r)
        print('PASS: diagnostic',name,flush=True)

if a.target!='cpu':
    root=a.output/'device-parser';root.mkdir(exist_ok=True)
    path=root/'main.k'
    path.write_text(prelude+'''fn onDevice(n:i64)->i64 ! ParseError:
  return try parseI64("42")
fn main()->unit ! (ParseError | IoError | TaskError):
  result=try parallel:
    gpu=devices.gpu()
    try await spawn onDevice(1) on gpu
  try println(format("{}",result))
  return ()
''')
    r=subprocess.run([a.compiler,path,'--target',a.target,'-o',root/'bad'],capture_output=True,timeout=120)
    (root/'build.log').write_bytes(r.stdout+r.stderr)
    assert r.returncode and b'error[G001]' in r.stderr and b'GPU' in r.stderr,(a.target,r)
    print('PASS: device parsing rejected',flush=True)
