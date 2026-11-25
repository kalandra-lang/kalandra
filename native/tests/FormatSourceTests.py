"""Run standard formatting through real source checking, lowering and execution."""
import argparse
import json
import os
from pathlib import Path
import subprocess

from ParallelTestRunner import run_parallel

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
point = '''from "std/fmt" import Display
type Point:
  x:i64
  y:i64
impl Display for Point:
  fn display(value:Self)->String:
    return format("({}, {})",value.x,value.y)
'''
ordered = '''from "std/fmt" import Display
from "std/io" import println
fn emit(text:String)->unit:
  return println(text) catch fn(error):return ()
type Mark:
  value:i64
impl Display for Mark:
  fn display(value:Self)->String:
    emit(format("display{}",value.value))
    return format("{}",value.value)
fn argument(value:i64)->Mark:
  emit(format("arg{}",value))
  return Mark {value:value}
'''
cases.update({
    'resumed-custom-order': (ordered+'''fn main()->String ! TaskError:
  return try parallel:
    format("{} {}",argument(1),try await spawn argument(2))
''', 'arg1\narg2\ndisplay1\ndisplay2\n1 2\n'),
    'custom-point': (point+'fn main()->String:\n  return format("Position: {}",Point {x:10,y:20})\n', 'Position: (10, 20)\n'),
    'controls': ('''fn main()->String:
  return format("{:.2}|{:>8}|{:<8}|{:x}|{:>8.2}|{:>6x}",3.14159,"hello","hello",255,1.125,-255)
''', '3.14|   hello|hello   |ff|    1.12|   -ff\n'),
    'unicode-alignment': ('''fn main()->String:
  return format("[{:>4}][{:<4}][{:>4}][{:>0}][{:>1}]","é","é","👩‍💻","","long")
''', '[   é][é  ][ 👩‍💻][][long]\n'),
    'custom-alignment': (point+'fn main()->String:\n  return format("[{:>12}][{:<12}]",Point {x:10,y:20},Point {x:1,y:2})\n', '[    (10, 20)][(1, 2)      ]\n'),
    'generic-display': ('''from "std/fmt" import Display
type Box[T]:
  value:T
impl[T:Display] Display for Box[T]:
  fn display(value:Self)->String:
    return format("<{}>",value.value)
fn render[T:Display](value:T)->String:
  return format("{:>8}",value)
fn main()->String:
  return render(Box {value:42})+":"+render(Box {value:"é"})
''', '    <42>:     <é>\n'),
    'enum-display': ('''from "std/fmt" import Display
enum Reading:
  Missing
  Value:
    number:f64
impl Display for Reading:
  fn display(value:Self)->String:
    return match value:
      Reading.Missing:"missing"
      Reading.Value {number}:format("{:.2}",number)
fn main()->String:
  return format("{}:{}",Reading.Missing,Reading.Value {number:1.375})
''', 'missing:1.38\n'),
    'display-alias': ('''from "std/fmt" import Display as Show, format as fmt
type Item:
  number:i64
impl Show for Item:
  fn display(value:Self)->String:
    return fmt("v={}",value.number)
fn main()->String:
  return fmt("{}",Item {number:42})
''', 'v=42\n'),
    'display-order-once': (ordered+'fn main()->String:\n  return format("{}:{}",argument(1),argument(2))\n', 'arg1\narg2\ndisplay1\ndisplay2\n1:2\n'),
    'display-argument-error': (ordered+'''error Stop:
  No
fn abort()->Mark ! Stop:
  emit("arg2")
  fail Stop.No
fn run()->String ! Stop:
  return format("{}:{}",argument(1),try abort())
fn main()->String:
  return run() catch fn(error):return "caught"
''', 'arg1\narg2\ncaught\n'),
    'primitive-generic': ('''fn render[T](value:T)->String:
  return format("{}",value)
fn main()->String:
  return render(42)+":"+render(1.5)
''', '42:1.5\n'),
    'large-precision': ('fn main()->i64:\n  return length(format("{:.1000000}",0.125))\n', '1000002\n'),
    'float-rounding': ('''fn main()->String:
  a:f32=1.125
  b:f32=-0.0
  return format("{:.2}|{:.2}|{:.0}|{:.0}|{:.2}|{:.2}",a,1.375,2.5,3.5,b,-1.125)
''', '1.12|1.38|2|4|-0.00|-1.12\n'),
    'integer-hex-edges': ('''fn main()->String:
  a:i8=-128
  b:i16=-32768
  c:i32=-2147483648
  d:i64=-9223372036854775808
  e:u8=255
  f:u16=65535
  g:u32=4294967295
  h:u64=18446744073709551615
  return format("{:x}|{:x}|{:x}|{:x}|{:x}|{:x}|{:x}|{:x}",a,b,c,d,e,f,g,h)
''', '-80|-8000|-80000000|-8000000000000000|ff|ffff|ffffffff|ffffffffffffffff\n'),
})
cases['display-never'] = ('''from "std/fmt" import Display
type Item:
  value:i64
impl Display for Item:
  fn display(value:Self)->String ! never:
    return format("{}",value.value)
fn main()->String:
  return format("{}",Item {value:42})
''', '42\n')
primitive_values = [('i8','-128'),('i16','-32768'),('i32','-2147483648'),
                    ('i64','-9223372036854775808'),('u8','255'),('u16','65535'),
                    ('u32','4294967295'),('u64','18446744073709551615'),
                    ('f32','1.5'),('f64','-0.0'),('bool','true'),('String','"é"')]
declarations = ''.join(f'  a{i}:{ty}={value}\n' for i,(ty,value) in enumerate(primitive_values))
rendered = ','.join(f'Display[{ty}].display(a{i})' for i,(ty,_) in enumerate(primitive_values))
cases['display-primitives'] = ('from "std/fmt" import Display\nfn main()->String:\n'+declarations+
                               '  return format("'+'|'.join(['{}']*12)+'",'+rendered+')\n',
                               cases['all-types'][1])

def build_format_batch(batch_name, batch_items, has_tasks):
    batch_dir = a.output / batch_name
    batch_dir.mkdir(exist_ok=True)
    (batch_dir / 'helper.k').write_text('export from "std/fmt" import format as fmt\n', encoding='utf-8')
    effect = '(IoError | TaskError)' if has_tasks else 'IoError'
    imports_lines = ['from "std/io" import println, IoError', 'from "std/fmt" import format']
    dispatch_lines = [f'fn main(args:[String])->unit ! {effect}:']
    for index, (name, (source, _)) in enumerate(batch_items):
        module_text = source if name == 'ordinary-format' else prelude + source
        assert 'fn main()->' in module_text, name
        module_text = module_text.replace('fn main()->', 'export fn run_case()->', 1)
        (batch_dir / f'case_{index}.k').write_text(module_text, encoding='utf-8')
        imports_lines.append(f'from "./case_{index}.k" import run_case as run_{index}')
        call_expr = f'try run_{index}()' if has_tasks else f'run_{index}()'
        printed = f'format("{{}}", {call_expr})' if name == 'large-precision' else call_expr
        dispatch_lines.append(f'  if args[0] == "{name}":\n    try println({printed})\n    return ()')
    dispatch_lines.append('  return ()\n')
    batch_main = batch_dir / 'main.k'
    batch_main.write_text('\n'.join(imports_lines) + '\n' + '\n'.join(dispatch_lines), encoding='utf-8')
    batch_exe = batch_dir / ('program.exe' if os.name == 'nt' else 'program')
    built = subprocess.run(list(map(str, [a.compiler, batch_main, *flags, '-o', batch_exe])),
                           capture_output=True, text=True, timeout=180, encoding='utf-8')
    (batch_dir / 'build.log').write_text(built.stdout + built.stderr, encoding='utf-8')
    assert built.returncode == 0, (batch_name, built.stdout, built.stderr)
    return batch_exe

seq_items = [(k, v) for k, v in cases.items() if '->String ! TaskError:' not in v[0]]
task_items = [(k, v) for k, v in cases.items() if '->String ! TaskError:' in v[0]]
batch_exes = dict(run_parallel(
    [('seq-batch', seq_items, False), ('task-batch', task_items, True)],
    lambda b: (b[0], build_format_batch(*b)),
))

def run_positive(item):
    name, (source, expected) = item
    exe = batch_exes['task-batch' if '->String ! TaskError:' in source else 'seq-batch']
    chunk = []
    for workers in ((1, 4) if name == 'cpu-task' else (1,)):
        run = subprocess.run([exe, name], capture_output=True, text=True, encoding='utf-8', timeout=30,
                             env=dict(os.environ, KALANDRA_WORKERS=str(workers)))
        record = {'case': name, 'workers': workers, 'compiled': True, 'status': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
        assert run.returncode == 0 and not run.stderr and run.stdout == expected, (name, record, expected)
        print('PASS:', name, workers, flush=True)
        chunk.append(record)
    return chunk

results = [rec for chunk in run_parallel(cases.items(), run_positive) for rec in chunk]
(a.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')

if a.variant=='debug':
    negatives={
        'template-value': ('fn main()->String:\n  template="{}"\n  return format(template,42)\n','S080'),
        'missing-template': ('fn main()->String:\n  return format()\n','S080'),
        'too-few': ('fn main()->String:\n  return format("{} {}",1)\n','S080'),
        'too-many': ('fn main()->String:\n  return format("{}",1,2)\n','S080'),
        'unsupported-field': ('fn main()->String:\n  return format("{:q}",42)\n','S080'),
        'array': ('fn main()->String:\n  return format("{}",[1,2])\n','S083'),
        'unit': ('fn main()->String:\n  return format("{}",())\n','S083'),
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
    negatives.update({
        'precision-integer': ('fn main()->String:\n  return format("{:.2}",42)\n','S080'),
        'hex-float': ('fn main()->String:\n  return format("{:x}",1.5)\n','S080'),
        'hex-bool': ('fn main()->String:\n  return format("{:x}",true)\n','S080'),
        'precision-custom': (point+'fn main()->String:\n  return format("{:.2}",Point {x:1,y:2})\n','S080'),
        'width-overflow': ('fn main()->String:\n  return format("{:>9223372036854775808}","a")\n','S080'),
        'precision-overflow': ('fn main()->String:\n  return format("{:.9223372036854775808}",1.5)\n','S080'),
        'negative-width': ('fn main()->String:\n  return format("{:>-1}","a")\n','S080'),
        'precision-required': ('fn main()->String:\n  return format("{:.}",1.5)\n','S080'),
        'missing-conformance': ('''type Item:
  number:i64
fn main()->String:
  return format("{}",Item {number:42})
''','S083'),
        'unrelated-display': ('''trait Display:
  fn display(value:Self)->String
type Item:
  number:i64
impl Display for Item:
  fn display(value:Self)->String:
    return "fake"
fn main()->String:
  return format("{}",Item {number:42})
''','S083'),
        'generic-bound-required': (point+'''fn render[T](value:T)->String:
  return format("{}",value)
fn main()->String:
  return render(Point {x:1,y:2})
''','S083'),
        'mutable-display': ('''from "std/fmt" import Display
type Item:
  number:i64
impl Display for Item:
  fn display(mut value:Self)->String:
    return "bad"
fn main()->String:
  return format("{}",Item {number:42})
''','S076'),
        'consuming-display': ('''from "std/fmt" import Display
type Item:
  number:i64
impl Display for Item:
  fn display(own value:Self)->String:
    return "bad"
fn main()->String:
  return format("{}",Item {number:42})
''','S081'),
        'wrong-display-result': ('''from "std/fmt" import Display
type Item:
  value:i64
impl Display for Item:
  fn display(value:Self)->i64:
    return value.value
fn main()->String:
  return format("{}",Item {value:42})
''','S083'),
        'fallible-display': ('''from "std/fmt" import Display
error Problem:
  Bad
type Item:
  value:i64
impl Display for Item:
  fn display(value:Self)->String ! Problem:
    fail Problem.Bad
fn main()->String:
  return format("{}",Item {value:42})
''','S083'),
        'display-mutates-input': ('''from "std/fmt" import Display
type Item:
  values:[i64]
impl Display for Item:
  fn display(value:Self)->String:
    value.values[0]=99
    return "bad"
fn main()->String:
  values=[1]
  return format("{}",Item {values:values})
''','S077'),
    })
    def check_negative(item):
        name,(source,code)=item
        folder=a.output/('reject-'+name)
        folder.mkdir(exist_ok=True)
        path=folder/'main.k'
        path.write_text(prelude+source, encoding='utf-8')
        output=folder/('program.exe' if os.name=='nt' else 'program')
        result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','main',*flags,'-o',output])),capture_output=True,text=True,timeout=30, encoding='utf-8')
        (folder/'diagnostic.log').write_text(result.stdout+result.stderr, encoding='utf-8')
        assert result.returncode and f'[{code}]' in result.stderr and not output.exists(),(name,result)
        print('PASS: rejected',name,code,flush=True)
    run_parallel(negatives.items(), check_negative)

if a.clang and a.runtime:
    folder=a.output/'generated-failure';folder.mkdir(exist_ok=True)
    path=folder/'main.k'
    path.write_text(prelude+'fn probe(divisor:i64)->String:\n  return format("{} {}",1,100/divisor)\nfn main()->String:\n  return probe(2)\n', encoding='utf-8')
    llvm=folder/'program.ll'
    result=subprocess.run(list(map(str,[a.compiler,path,'--test-entry','probe','--emit-llvm',llvm,*flags,'--no-link'])),capture_output=True,timeout=180)
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
