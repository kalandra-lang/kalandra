"""Run standard formatting through real source checking, lowering and execution."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--clang', type=Path)
parser.add_argument('--runtime', type=Path)
parser.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), required=True)
a = parser.parse_args()
a.compiler = a.compiler.resolve()
a.output.mkdir(parents=True, exist_ok=True)
flags = {'debug':['-O0'], 'optimized':['-O2'], 'sanitized':['--sanitize'], 'thread-sanitized':['--thread-sanitize']}[a.variant]
prelude = 'from "std/fmt" import format\n'
cases = {
    'all-types': ('''fn main()->String:
  a:i8=-128
  b:i16=-32768
  c:i32=-2147483648
  d:i64=-9223372036854775808
  e:u8=255
  f:u16=65535
  g:u32=4294967295
  h:u64=18446744073709551615
  i:f32=1.5
  j:f64=-0.0
  return format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", a,b,c,d,e,f,g,h,i,j,true,"é")
''', '-128|-32768|-2147483648|-9223372036854775808|255|65535|4294967295|18446744073709551615|1.5|-0|true|é\n'),
    'ordinary-format': ('fn format(value:String)->String:\n  return value\nfn main()->String:\n  template="ordinary"\n  return format(template)\n', 'ordinary\n'),
    'alias': ('from "std/fmt" import format as fmt\nfn main()->String:\n  return fmt("{}",42)\n', '42\n'),
    'reexport': ('from "./helper.k" import fmt\nfn main()->String:\n  return fmt("{}",42)\n', '42\n'),
    'empty': ('fn main()->String:\n  return format("")\n', '\n'),
    'escapes': ('fn main()->String:\n  return format("{{{}}} {{}}",42)\n', '{42} {}\n'),
    'embedded-nul': ('fn main()->String:\n  return format("α\\0{}", "é")\n', 'α\0é\n'),
    'nested': ('fn main()->String:\n  return format("<{}>", format("{}:{}",1,false))\n', '<1:false>\n'),
    'sequential-mutation': ('''fn increment(mut values:[i64])->i64:
  values[0]=values[0]+1
  return values[0]
fn main()->String:
  values=[0]
  return format("{} {}", increment(values), increment(values))
''', '1 2\n'),
    'map': ('''fn decorate(value:i64)->String:
  return format("<{}>",value)
fn main()->String:
  values=map([1,2,3],decorate)
  return values[0]+values[1]+values[2]
''', '<1><2><3>\n'),
    'closure': ('''fn prefix(text:String)->fn(i64)->String:
  return fn(value:i64)->String:
    return format("{}{}",text,value)
fn main()->String:
  decorate=prefix("value=")
  return decorate(42)
''', 'value=42\n'),
    'resumed-format': ('''fn identity(value:i64)->i64:
  return value
fn main()->String ! TaskError:
  return try parallel:
    task=spawn identity(42)
    value=try await task
    suffix="end"
    format("{}:{}:{}",value,false,suffix)
''', '42:false:end\n'),
    'cpu-task': ('''fn render(value:i64)->String:
  return format("value={}",value)
fn main()->String ! TaskError:
  return try parallel:
    result=spawn render(42)
    try await result
''', 'value=42\n'),
}
results=[]
for name,(source,expected) in cases.items():
    folder=a.output/name
    folder.mkdir(exist_ok=True)
    path=folder/'main.k'
    path.write_text(source if name=='ordinary-format' else prelude+source, encoding='utf-8')
    if name=='reexport':
        (folder/'helper.k').write_text('export from "std/fmt" import format as fmt\n', encoding='utf-8')
    output=folder/('program.exe' if os.name=='nt' else 'program')
    command=[a.compiler,path,'--test-entry','main',*flags,'-o',output]
    built=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180, encoding='utf-8')
    (folder/'build.log').write_text(built.stdout+built.stderr, encoding='utf-8')
    assert built.returncode==0,(name,built.stdout,built.stderr)
    for workers in ((1,4) if name=='cpu-task' else (1,)):
        run=subprocess.run([output],capture_output=True,text=True,encoding='utf-8',timeout=30,
                           env=dict(os.environ,KALANDRA_WORKERS=str(workers)))
        record={'case':name,'workers':workers,'compiled':True,'status':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
        results.append(record)
        (a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n', encoding='utf-8')
        assert run.returncode==0 and not run.stderr and run.stdout==expected,(name,record,expected)
        print('PASS:',name,workers,flush=True)

if a.variant=='debug':
    negatives={
        'template-value': ('fn main()->String:\n  template="{}"\n  return format(template,42)\n','S080'),
        'missing-template': ('fn main()->String:\n  return format()\n','S080'),
        'too-few': ('fn main()->String:\n  return format("{} {}",1)\n','S080'),
        'too-many': ('fn main()->String:\n  return format("{}",1,2)\n','S080'),
        'unsupported-field': ('fn main()->String:\n  return format("{:x}",42)\n','S080'),
        'array': ('fn main()->String:\n  return format("{}",[1,2])\n','S080'),
        'unit': ('fn main()->String:\n  return format("{}",())\n','S080'),
        'reference': ('fn main()->String:\n  f=format\n  return f("x")\n','S080'),
        'spawn': ('fn main()->String ! TaskError:\n  return try parallel:\n    try await spawn format("{}",42)\n','S080'),
        'pending-array-reader': ('''fn read(values:[i64])->i64:
  return values[0]
fn edit(mut values:[i64])->i64:
  values[0]=2
  return values[0]
fn main()->String ! TaskError:
  return try parallel:
    values=[1]
    task=spawn read(values)
    text=format("{}",edit(values))
    try await task
    text
''','S077'),
    }
    for name,(source,code) in negatives.items():
        folder=a.output/('reject-'+name)
        folder.mkdir(exist_ok=True)
        path=folder/'main.k'
        path.write_text(prelude+source, encoding='utf-8')
        output=folder/('program.exe' if os.name=='nt' else 'program')
        result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','main',*flags,'-o',output])),capture_output=True,text=True,timeout=30, encoding='utf-8')
        (folder/'diagnostic.log').write_text(result.stdout+result.stderr, encoding='utf-8')
        assert result.returncode and f'[{code}]' in result.stderr and not output.exists(),(name,result)
        print('PASS: rejected',name,code,flush=True)

if a.clang and a.runtime:
    folder=a.output/'generated-failure';folder.mkdir(exist_ok=True)
    path=folder/'main.k'
    path.write_text(prelude+'fn probe(divisor:i64)->String:\n  return format("{} {}",1,100/divisor)\nfn main()->String:\n  return probe(2)\n', encoding='utf-8')
    llvm=folder/'program.ll'
    exe=folder/('program.exe' if os.name=='nt' else 'program')
    result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','probe','--emit-llvm',llvm,*flags,'-o',exe])),capture_output=True,timeout=180)
    assert result.returncode==0,result
    native_flags={'debug':['-O0'],'optimized':['-O2'],'sanitized':['-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all'],'thread-sanitized':['-O1','-fsanitize=thread']}[a.variant]
    driver=folder/('driver.exe' if os.name=='nt' else 'driver')
    result=subprocess.run(list(map(str,[a.clang,'-std=c++23','-Wno-override-module',*native_flags,Path(__file__).with_name('FormatGeneratedTests.cpp'),llvm,a.runtime,'-o',driver])),capture_output=True,timeout=180)
    (folder/'driver-build.log').write_bytes(result.stdout+result.stderr)
    assert result.returncode==0,result
    result=subprocess.run([driver],capture_output=True,timeout=30)
    (folder/'driver.log').write_bytes(result.stdout+result.stderr)
    assert result.returncode==0 and not result.stderr,result
    print(result.stdout.decode(),end='',flush=True)
