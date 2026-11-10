"""Public lexical paths through normal CPU and mixed GPU applications."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--variant',choices=('debug','optimized','sanitized','thread-sanitized'),required=True)
p.add_argument('--target',choices=('cpu','metal','vulkan','cuda'),default='cpu')
p.add_argument('--compile-only',action='store_true')
a=p.parse_args();a.compiler=a.compiler.resolve();a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
flag={'debug':'-O0','optimized':'-O2','sanitized':'--sanitize','thread-sanitized':'--thread-sanitize'}[a.variant]
windows=os.name=='nt';sep='\\' if windows else '/'
prelude='''from "std/path" import join, fileName, parentPath, extension, isAbsolute
from "std/io" import println, IoError
from "std/fmt" import format
'''
def build(name,source,diagnostic=None,with_prelude=True):
    root=a.output/name;root.mkdir(exist_ok=True);path=root/'main.k';path.write_text((prelude if with_prelude else '')+source,encoding='utf-8')
    executable=root/('program.exe' if windows else 'program');output=executable if a.target=='cpu' else root/'bundle'
    command=[a.compiler,path,flag,'-o',output]
    if a.target!='cpu':command.extend(['--target',a.target])
    r=subprocess.run(command,capture_output=True,timeout=240)
    (root/'build.log').write_bytes(r.stdout+r.stderr)
    if diagnostic:
        assert r.returncode and diagnostic in r.stderr,(name,r)
        return None
    assert not r.returncode,(name,r)
    return executable if a.target=='cpu' else output/executable.name

def run(name,source,expected,expected_status=0):
    if a.target!='cpu':
        source=source.replace('! IoError:','! (IoError | TaskError):').replace('! (IoError | FsError):','! (IoError | FsError | TaskError):')
        source=re.sub(r'(fn main\([^\n]+\n)',r'\1  answer=try parallel:\n    gpu=devices.gpu()\n    try await spawn double(21) on gpu\n  try println(format("{}",answer))\n',source)
        source='fn double(n:i64)->i64:\n  return n*2\n'+source;expected='42\n'+expected
    executable=build(name,source)
    if a.compile_only:print('PASS: compiled',name,flush=True);return
    for workers in (1,4):
        with tempfile.TemporaryDirectory(prefix='path app ',dir=a.output) as work:
            r=subprocess.run([executable],cwd=work,capture_output=True,timeout=60,env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
        (executable.parent/f'run-{workers}.log').write_bytes(r.stdout+r.stderr)
        stderr=r.stderr
        if a.target=='metal':stderr=b''.join(x for x in stderr.splitlines(keepends=True) if not re.fullmatch(rb'\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+ program\[\d+:\d+\] Metal API Validation Enabled\r?\n?',x))
        assert r.returncode==expected_status and r.stdout==expected.encode() and not stderr,(name,workers,r)
    print('PASS: executed',name,flush=True)

run('all-functions','''fn main()->unit ! IoError:
  path=join(["data","runs","output.csv"])
  try println(path)
  try println(fileName(path))
  try println(parentPath(path))
  try println(extension(path))
  try println(format("{}",isAbsolute(path)))
  return ()
''',f'data{sep}runs{sep}output.csv\noutput.csv\ndata{sep}runs\n.csv\nfalse\n')

run('components','''fn main()->unit ! IoError:
  for text in ["", ".gitignore", ".config.json", "archive.tar.gz", "file.", "...", "a/../b/"]:
    try println(format("{}|{}|{}|{}",fileName(text),parentPath(text),extension(text),isAbsolute(text)))
  try println(join([]))
  return ()
''','|.||false\n.gitignore|.||false\n.config.json|.|.json|false\narchive.tar.gz|.|.gz|false\nfile.|.|.|false\n...|.||false\nb|a/..||false\n\n')

run('platform-rules',r'''fn main()->unit ! IoError:
  for text in ["/root", "C:\\root", "C:leaf", "\\leaf", "\\\\server\\share\\file", "\\\\?\\C:\\dir/file.txt"]:
    try println(format("{}:{}:{}",fileName(text),extension(text),isAbsolute(text)))
  return ()
''',('root::false\nroot::true\nleaf::false\nleaf::false\nfile::true\ndir/file.txt:.txt:true\n' if windows else 'root::true\nC:\\root::false\nC:leaf::false\n\\leaf::false\n\\\\server\\share\\file::false\nfile.txt:.txt:false\n'))

run('copy-slice','''fn main()->unit ! IoError:
  original=["skip","data","file.txt"]
  parts=copy(original)
  selected=slice(parts,1,3)
  try println(join(selected))
  try println(fileName(join(copy(selected))))
  return ()
''',f'data{sep}file.txt\nfile.txt\n')

run('function-values','''fn apply(make:fn([String])->String,parts:[String])->String:
  return make(parts)
fn project(pick:fn(String)->String,text:String)->String:
  return pick(text)
fn main()->unit ! IoError:
  make=join
  path=apply(make,["one","two","result.txt"])
  try println(project(fileName,path))
  try println(project(extension,path))
  return ()
''','result.txt\n.txt\n')

run('tasks','''fn work(parts:[String])->String:
  return fileName(join(parts))
fn main()->unit ! (IoError | TaskError):
  parts=["source","temporary","value.txt"]
  result=try parallel:
    job=spawn work(parts)
    try await job
  try println(result)
  return ()
''','value.txt\n')

run('temporary-return','''fn make()->String:
  return join(["prefix",".cache","résumé.csv"])
fn main()->unit ! IoError:
  value=make()
  for i in range(30):
    temporary=join(["throwaway","..","other.bin"])
    try println(extension(temporary))
  try println(fileName(value))
  try println(parentPath(value))
  return ()
''','.bin\n'*30+f'résumé.csv\nprefix{sep}.cache\n')

run('filesystem','''from "std/fs" import open, OpenMode, FsError, writeText, read, close
fn main()->unit ! (IoError | FsError):
  path=join([".","path-example.txt"])
  file=try open(path,OpenMode.Create)
  try writeText(file,"hello")
  try close(file)
  input=try open(path,OpenMode.Read)
  bytes=try read(input,5)
  try close(input)
  try println(format("{}:{}:{}",fileName(path),extension(path),length(bytes)))
  return ()
''','path-example.txt:.txt:5\n')

if a.target=='cpu':
    # No console/parallel use: ordinary nonfallible functions and process status.
    source='''from "std/path" import join, fileName, isAbsolute
from "std/process" import ExitStatus
fn main()->ExitStatus:
  name=fileName(join(["a","b.txt"]))
  if name=="b.txt":
    return ExitStatus {code:42}
  return ExitStatus {code:1}
'''
    executable=build('pure-application',source,with_prelude=False)
    if not a.compile_only:
        r=subprocess.run([executable],capture_output=True,timeout=60)
        assert r.returncode==42 and not r.stdout and not r.stderr,r
    for name,source,diagnostic in (
        ('join-scalar','fn main()->unit:\n  text=join("x")\n  return ()\n',b'error[S010]'),
        ('join-numeric','fn main()->unit:\n  text=join([1,2])\n  return ()\n',b'error[S013]'),
        ('wrong-input','fn main()->unit:\n  text=fileName(12)\n  return ()\n',b'error[S013]'),
        ('wrong-result','fn main()->unit:\n  text:i64=isAbsolute("/x")\n  return ()\n',b'error[S010]'),
        ('private-intrinsic','fn main()->unit:\n  text=__kalandra_path_join(["x"])\n  return ()\n',b'error[S031]')):
        build(name,source,diagnostic);print('PASS: diagnostic',name,flush=True)
else:
    build('device-path','''fn onDevice(n:i64)->bool:
  return isAbsolute("/x")
fn main()->unit ! (IoError | TaskError):
  result=try parallel:
    gpu=devices.gpu()
    try await spawn onDevice(1) on gpu
  try println(format("{}",result))
  return ()
''',b'error[G001]')
    print('PASS: device path operation rejected',flush=True)
