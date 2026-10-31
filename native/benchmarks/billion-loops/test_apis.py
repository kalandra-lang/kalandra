"""Prototype API correctness and failure checks; timings are deliberately absent."""
import ast,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
COMPILER=ROOT/'build/native/native/kalandra'
CHECKER=ROOT/'build/native/native/kalandra-check'
records=[]
def command(args,expected=0):
 p=subprocess.run(list(map(str,args)),text=True,capture_output=True,timeout=90)
 assert p.returncode==expected,(args,p.returncode,p.stdout,p.stderr)
 return p
with tempfile.TemporaryDirectory(prefix='kalandra input APIs ') as folder:
 work=Path(folder)
 def build(name,body,opt='-O2'):
  source=work/(name+'.k');source.write_text(body)
  binary=work/(name+opt)
  command([COMPILER,source,opt,'-o',binary]);return binary
 for opt in ('-O0','-O2','--sanitize'):
  arg=build('args','fn main()->int32:\n  args.int32(0)\n',opt)
  for value in ('0','-42','2147483647','-2147483648'):
   assert command([arg,value]).stdout.strip()==value
  for values in ([],[''],['hello'],['12x'],['2147483648'],['-2147483649']):
   p=command([arg,*values],86);assert 'args.int32:' in p.stderr
  negative=build('negative-arg','fn main()->int32:\n  args.int32(-1)\n',opt)
  assert 'BoundsFault' in command([negative,'42'],86).stderr
  gen=build('generate','fn main()->[int32]:\n  offset=args.int32(1)\n  generate(int(args.int32(0)),fn(i:int)->int32: int32(i)+offset)\n',opt)
  for n in (0,1,7,31):
   assert ast.literal_eval(command([gen,'--',str(n),'17']).stdout)==[i+17 for i in range(n)]
  assert 'BoundsFault' in command([gen,'--','-1','0'],86).stderr
  empty=build('empty','fn main()->int:\n  length(generate(0,fn(i:int)->int32: args.int32(99)))\n',opt)
  assert command([empty]).stdout.strip()=='0'
  overflow=build('huge','fn main()->int:\n  length(generate(9223372036854775807,fn(i:int)->int32: int32(i)))\n',opt)
  assert 'Fault' in command([overflow],86).stderr
  random=build('random','fn main()->[int32]:\n  generate(1000,fn(i:int)->int32: random.int32(-2,3))\n',opt)
  values=ast.literal_eval(command([random,'--all']).stdout)
  assert len(values)==1000 and set(values)<=set(range(-2,3)) and len(set(values))>1
  for lo,hi in ((0,0),(2,-1)):
   invalid=build('bad-random',f'fn main()->int32:\n  random.int32({lo},{hi})\n',opt)
   assert 'BoundsFault' in command([invalid],86).stderr
  partial=build('callback-fault','fn main()->int:\n  length(generate(5,fn(i:int)->int32: args.int32(i)))\n',opt)
  assert 'BoundsFault' in command([partial,'1','2'],86).stderr
  records.append(dict(mode=opt,argumentBoundaries=True,generationOrder=True,emptyNoCallback=True,sizeFault=True,randomRangeAndDistinctCalls=True,callbackFailure=True))
 for name,source,expected in (
  ('strings','fn main()->[String]:\n  generate(3,fn(i:int)->String: "x")\n','["x", "x", "x"]'),
  ('bools','fn main()->[Bool]:\n  generate(3,fn(i:int)->Bool: i>0)\n','[false, true, true]'),
  ('nested','fn main()->[int]:\n  generate(4,fn(i:int)->int: length(generate(i,fn(j:int)->int: j)))\n','[0, 1, 2, 3]'),
  ('named','fn identity(i:int)->int:\n  i\nfn main()->[int]:\n  generate(4,identity)\n','[0, 1, 2, 3]')):
  exe=build(name,source)
  assert command([exe]).stdout.strip()==expected,(name,command([exe]).stdout)
 task_source='fn worker(i:int)->int32:\n  values=generate(i,fn(j:int)->int32: args.int32(0)+random.int32(0,1))\n  int32(length(values))\nfn main()->[int32] !TaskError:\n  try parallel:\n    try parallelMap([0,1,2,3],worker)\n'
 for opt in ('-O2','--thread-sanitize'):
  task=build('tasks',task_source,opt)
  assert command([task,'--','17']).stdout.strip()=='[0, 1, 2, 3]'
 for name,source in (
  ('bad-index-type','fn main()->[int32]:\n  generate(4,fn(i:int32)->int32: i)\n'),
  ('bad-count','fn main()->[int]:\n  generate(4.0,fn(i:int)->int: i)\n'),
  ('bad-arity','fn main()->int32:\n  random.int32(0)\n'),
  ('bad-args-type','fn main()->int32:\n  args.int32(1.0)\n')):
  file=work/(name+'.k');file.write_text(source)
  command([CHECKER,file],1)
 placement=work/'placement.k';placement.write_text('fn main()->int32:\n  args.int32(0)\n')
 command([CHECKER,'--placement',placement],1)
print(json.dumps(dict(modes=records,genericElements=['String','Bool','nested generation','named callback'],rejectedTypesAndPlacement=True),indent=2))
