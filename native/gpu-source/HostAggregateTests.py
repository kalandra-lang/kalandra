"""Owned native records/enums around GPU work, with equivalent CPU source oracles.

CPU coordination retains native aggregates. Eligible whole-record captures are
reconstructed on the GPU; their source arithmetic/traversal executes there.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler', type=Path, required=True)
p.add_argument('--backend', choices=('metal', 'vulkan', 'cuda'), required=True)
p.add_argument('--variant', choices=('debug', 'optimized', 'sanitized', 'thread-sanitized'), default='optimized')
p.add_argument('--root', type=Path, required=True)
p.add_argument('--compile-only', action='store_true')
p.add_argument('--case', action='append', default=[], help='Run named discovery cases; omit for the full qualification corpus')
a = p.parse_args()
a.compiler = a.compiler.resolve()
a.root = a.root.resolve()
a.root.mkdir(parents=True, exist_ok=True)
flags = {'debug': ['-O0'], 'optimized': ['-O2'], 'sanitized': ['--sanitize'],
         'thread-sanitized': ['--thread-sanitize']}[a.variant]
suffix = '.exe' if os.name == 'nt' else ''
def run(command, env=None):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240, env=env)
    assert r.returncode == 0, (command, r.returncode, r.stdout, r.stderr)
    assert not any(s in r.stderr for s in ('AddressSanitizer', 'ThreadSanitizer', 'runtime error:')), r.stderr
    return r.stdout.strip()
work = 'fn work(n:int)->int:\n  total=0\n  for i in range(n):\n    total=total+i\n  total\n'
cases = {}
for typ in ('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64','Bool'):
    if typ == 'Bool':
        fields = 'Batch {value:true,values:[true,false,true]}'
        score = '  if batch.value and batch.values[0] and not batch.values[1]:6\n  else:0\n'
    else:
        fields = 'Batch {value:2,values:[1,2,3]}'
        score = '  int(batch.value)+int(batch.values[0])+int(batch.values[2])\n'
    helper = f'type Batch:\n  value:{typ}\n  values:[{typ}]\nfn make()->Batch:\n  {fields}\nfn score(batch:Batch)->int:\n' + score
    for child in (False, True):
        setup = '    batch=try await spawn make() on devices.cpu()\n' if child else '    batch=make()\n'
        cases[('child-' if child else 'direct-')+typ] = (helper+work+'fn main()->int ! TaskError:\n  try parallel:\n'+setup+'    try await spawn work(score(batch)) on devices.gpu()\n',15)
    cases['entry-'+typ] = (helper+'''fn device(batch:Batch)->int:
  total=0
  for i in range(score(batch)):
    total=total+i
  total
fn main()->int ! TaskError:
  batch=make()
  try parallel:
    try await spawn device(batch) on devices.gpu()
''',15)
common = 'type Batch:\n  values:[int]\n  enabled:Bool\nfn make(n:int)->Batch:\n  Batch {values:[n,n+1],enabled:true}\nfn score(batch:Batch)->int:\n  if batch.enabled:batch.values[0]+batch.values[1]\n  else:0\n'
cases['retained-through-replacement'] = (common+work+'''fn main()->int ! TaskError:
  saved=make(7)
  for i in range(1000):
    temporary=make(i)
  try parallel:
    try await spawn work(score(saved)) on devices.gpu()
''',105)
cases['record-update-shared-array'] = (common+work+'''fn main()->int ! TaskError:
  original=make(1)
  updated=original with {enabled:false}
  original.values[0]=7
  try parallel:
    try await spawn work(updated.values[0]+updated.values[1]) on devices.gpu()
''',36)
cases['helper-record-capture-result'] = (common+work+'''fn remote(batch:Batch)->Batch ! TaskError:
  try parallel:
    n=try await spawn work(score(batch)) on devices.gpu()
    make(n)
fn main()->int ! TaskError:
  batch=make(1)
  try parallel:
    copied=try await spawn remote(batch) on devices.cpu()
    copied.values[0]+copied.values[1]
''',7)
enum = '''enum Choice:
  Empty
  Some:
    batch:Batch
fn pick(n:int)->Choice:
  if n==0:Choice.Empty
  else:Choice.Some {batch:make(n)}
fn scoreChoice(value:Choice)->int:
  match value:
    Choice.Empty:4
    Choice.Some {batch}:score(batch)
'''
cases['repeated-record-entry'] = (common+'''fn main()->int ! TaskError:
  try parallel:
    first=spawn score(make(2)) on devices.gpu()
    second=spawn score(make(9)) on devices.gpu()
    left=try await first
    right=try await second
    left*100+right
''',519)
for n, score in [(0,4),(3,7)]:
    cases['enum-child-'+str(n)] = (common+enum+work+f'''fn main()->int ! TaskError:
  try parallel:
    choice=try await spawn pick({n}) on devices.cpu()
    n=match choice:
      Choice.Empty:4
      Choice.Some {{batch}}:score(batch)
    try await spawn work(n) on devices.gpu()
''',score*(score-1)//2)
cases['failed-enum-result-recovery'] = (common+enum+work+'''fn broken(n:int)->int:
  1/n
fn remote(n:int)->Choice ! TaskError:
  try parallel:
    done=try await spawn broken(n) on devices.gpu()
    pick(done)
fn main()->int ! TaskError:
  try parallel:
    outcome=parallel:
      choice=try await spawn remote(0) on devices.cpu()
      choice
    recovered=outcome catch fn(error):
      Choice.Some {batch:make(2)}
    try await spawn work(scoreChoice(recovered)) on devices.gpu()
''',10)
cases['recursive-native-projection-identity'] = (work+'''enum List:
  End
  Next:
    value:int
    tail:List
fn makeList(n:int)->List:
  if n==0:List.End
  else:List.Next {value:n,tail:makeList(n-1)}
fn sumList(list:List)->int:
  match list:
    List.End:0
    List.Next {value,tail}:value+sumList(tail)
fn forwardList(list:List)->List:
  list
fn main()->int ! TaskError:
  original=makeList(20)
  updated=List.Next {value:2,tail:original}
  try parallel:
    kept=try await spawn forwardList(updated) on devices.cpu()
    try await spawn work(sumList(kept)) on devices.gpu()
''',22366)
cases['native-owned-string-field'] = (work+'''type Message:
  text:String
fn makeMessage()->Message:
  Message {text:"Kalandra"}
fn scoreMessage(message:Message)->int:
  length(message.text)
fn main()->int ! TaskError:
  try parallel:
    message=try await spawn makeMessage() on devices.cpu()
    try await spawn work(scoreMessage(message)) on devices.gpu()
''',28)
cases['native-owned-record-array-field'] = (work+'''type Row:
  values:[int]
type Batch:
  rows:[Row]
fn makeBatch()->Batch:
  Batch {rows:[Row {values:[1,2]},Row {values:[3,4]}]}
fn scoreBatch(batch:Batch)->int:
  batch.rows[0].values[0]+batch.rows[1].values[1]
fn main()->int ! TaskError:
  try parallel:
    batch=try await spawn makeBatch() on devices.cpu()
    try await spawn work(scoreBatch(batch)) on devices.gpu()
''',10)
cases['device-array-result-native-record-field'] = (common+'''fn squares(values:[int])->[int]:
  generate(length(values),fn(i):values[i]*values[i])
fn main()->int ! TaskError:
  original=make(2)
  try parallel:
    values=try await spawn squares(original.values) on devices.gpu()
    wrapped=Batch {values:values,enabled:true}
    score(wrapped)
''',13)
cases['native-record-result-retains-original-alias'] = (common+work+'''fn forward(batch:Batch)->Batch:
  batch
fn main()->int ! TaskError:
  original=make(1)
  try parallel:
    alias=try await spawn forward(original) on devices.cpu()
    original.values[0]=9
    try await spawn work(alias.values[0]+alias.values[1]) on devices.gpu()
''',55)
cases['independent-copy-mutation-while-record-read-locked'] = (common+work+'''fn main()->int ! TaskError:
  original=make(1)
  fresh=copy(original.values)
  try parallel:
    pending=spawn score(original) on devices.cpu()
    fresh[0]=9
    n=try await pending
    try await spawn work(n+fresh[0]) on devices.gpu()
''',66)
cases['entry-returned-field-retains-original-alias'] = (common+'''fn device(batch:Batch)->[int]:
  batch.values
fn main()->int ! TaskError:
  original=make(2)
  try parallel:
    alias=try await spawn device(original) on devices.gpu()
    original.values[0]=9
    alias[0]+alias[1]
''',12)
nested = '''enum Inner:
  Empty
  Data:
    values:[int]
enum Outer:
  Empty
  Data:
    inner:Inner
fn device(value:Outer)->int:
  match value:
    Outer.Empty:7
    Outer.Data {inner}:
      match inner:
        Inner.Empty:11
        Inner.Data {values}:values[0]+values[1]
'''
for name,value,expected in [('outer-empty','Outer.Empty',7),
                            ('inner-empty','Outer.Data {inner:Inner.Empty}',11),
                            ('inner-data','Outer.Data {inner:Inner.Data {values:[2,3]}}',5)]:
    cases['entry-'+name] = (nested+f'''fn main()->int ! TaskError:
  original={value}
  try parallel:
    try await spawn device(original) on devices.gpu()
''',expected)
cases['entry-repeated-nested-alias-and-scalar'] = (common+'''type Pair:
  left:Batch
  right:Batch
fn device(value:Pair,values:[int],scale:int)->int:
  total=0
  for n in value.left.values:
    total=total+n
  total+value.right.values[0]+values[1]+scale
fn main()->int ! TaskError:
  batch=make(2)
  pair=Pair {left:batch,right:batch}
  try parallel:
    try await spawn device(pair,batch.values,7) on devices.gpu()
''',17)
fixtures = Path(__file__).resolve().parent.parent / 'tests/fixtures/mixed-host-aggregates'
cases['gpu-host-closure-empty'] = ('''fn work(values:[int],callback:fn(int)->int)->int:
  length(values)+callback(5)
fn main()->int ! TaskError:
  values=generate(0,fn(i:int):i)
  callback=fn(n:int):n+length(values)
  try parallel:
    try await spawn work(values,callback) on devices.gpu()
''', 5)
for typ in ('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64'):
    cases['gpu-host-closure-tensor-'+typ] = (f'''fn make(n:{typ})->fn(int)->{typ}:
  grid=tensor([n,n,n,n],[2,2])
  fn(i:int):grid[i,1]
fn work(callback:fn(int)->{typ})->int:
  int(callback(1))
fn main()->int ! TaskError:
  callback=make(7)
  try parallel:
    try await spawn work(callback) on devices.gpu()
''', 7)
cases['gpu-host-closure-resident-result'] = ((fixtures/'gpu-host-closure-resident-result.k').read_text(), 32)
cases['gpu-host-closure-record-result'] = ((fixtures/'gpu-host-closure-record-result.k').read_text(), 44)
cases['gpu-host-closure-array-result'] = ((fixtures/'gpu-host-closure-array-result.k').read_text(), 66)
cases['gpu-host-closure-array-result-slice'] = (
    (fixtures/'gpu-host-closure-array-result.k').read_text()
    .replace('values=[7,11]', 'values=[5,7,11,13]')
    .replace('callback=make(values)', 'callback=make(slice(values,1,3))')
    .replace('  values[0]+result[1]', '  values[1]+result[1]'), 54)
cases['gpu-host-closure-tensor-record'] = ((fixtures/'gpu-host-closure-tensor-record.k').read_text(), 7)
cases['gpu-host-closure-tensor'] = ((fixtures/'gpu-host-closure-tensor.k').read_text(), 20)
cases['gpu-host-closure-joined'] = ((fixtures/'gpu-host-closure-joined.k').read_text(), 42)
cases['gpu-host-closure'] = ((fixtures/'gpu-host-closure.k').read_text(), 22)
cases['gpu-host-closure-nested'] = ((fixtures/'gpu-host-closure-nested.k').read_text(), 26)
cases['gpu-host-closure-recursive-enum'] = ('''enum Tree:
  End
  Node:
    values:[int]
    tail:Tree
fn sumTree(tree:Tree)->int:
  match tree:
    Tree.End:0
    Tree.Node {values,tail}:values[0]+sumTree(tail)
fn make(values:[int])->fn(int)->int:
  tree=Tree.Node {values:values,tail:Tree.Node {values:values,tail:Tree.End}}
  fn(n:int):sumTree(tree)+n
fn work(callback:fn(int)->int)->int:
  callback(4)
fn main()->int ! TaskError:
  values=[9,11]
  callback=make(values)
  try parallel:
    result=try await spawn work(callback) on devices.gpu()
    values[0]=10
    result+values[0]
''',32)

cases['host-closure'] = ((fixtures/'host-closure.k').read_text(), 59)
cases['host-closure-record'] = ((fixtures/'host-closure-record.k').read_text(), 23)
cases['result-enum-owned-aliases'] = ((fixtures/'record-result.k').read_text(), 983)
cases['local-recursive-list'] = ((fixtures/'list-local.k').read_text(), 18)
cases['local-recursive-tree'] = ((fixtures/'tree-local.k').read_text(), 50275)
cases['local-recursive-record-array'] = ((fixtures/'array-local.k').read_text(), 46369)
cases['local-recursive-array-composition'] = ((fixtures/'array-composition.k').read_text(), 455)
cases['local-recursive-array-bounds'] = ((fixtures/'array-bounds.k').read_text(), 342)
cases['graph-entry-list'] = ((fixtures/'list-entry.k').read_text(), 18)
cases['graph-entry-record-array'] = ((fixtures/'array-graph-entry.k').read_text(), 27)
cases['graph-entry-copy'] = ((fixtures/'graph-entry-copy.k').read_text(), 10)
cases['graph-result-list'] = ((fixtures/'graph-result.k').read_text(), 18)
cases['graph-result-shared-alias'] = ((fixtures/'graph-result-alias.k').read_text(), 1000)
cases['graph-result-captured-alias'] = ((fixtures/'graph-result-captured.k').read_text(), 928)
cases['graph-result-captured-box'] = ((fixtures/'graph-result-captured-box.k').read_text(), 1008)
cases['graph-entry-multiple-captures'] = ((fixtures/'graph-entry-multiple-captures.k').read_text(), 84)
cases['graph-result-multiple-captures'] = ((fixtures/'graph-result-multiple-captures.k').read_text(), 1000)
cases['device-recursive-owned-array'] = ((fixtures/'device-recursive-owned-array.k').read_text(), 5057)
cases['device-recursive-bounds'] = ((fixtures/'device-recursive-bounds.k').read_text(), 2142)
cases['graph-result-recursive-capture'] = ((fixtures/'graph-result-recursive-capture.k').read_text(), 5059)
cases['device-array-closure'] = ((fixtures/'device-array-closure.k').read_text(), 239)
cases['device-closure-float64'] = ((fixtures/'device-closure-float64.k').read_text(), 6)
cases['device-nested-closure'] = ((fixtures/'device-nested-closure.k').read_text(), 239)
cases['device-tensor-arithmetic'] = ((fixtures/'device-tensor-arithmetic.k').read_text(), 1350)
cases['device-tensor-edge'] = ((fixtures/'device-tensor-edge.k').read_text(), 8)
cases['device-tensor-index'] = ((fixtures/'device-tensor-index.k').read_text(), 7)
cases['device-tensor-rank0'] = ((fixtures/'device-tensor-rank0.k').read_text(), 4)
for fixture, expected in {
    'device-tensor-resident-mixed': 17,
    'device-tensor-resident-operations': 98,
    'device-tensor-resident-alias': 609,
    'device-tensor-resident-graph': 88,
    'device-tensor-resident-rank8': 40,
    'device-tensor-return-edge': 10,
}.items():
    cases[fixture] = ((fixtures/f'{fixture}.k').read_text(), expected)
for typ in ('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64'):
    cases[f'device-tensor-return-{typ}'] = (f'''fn make(rows:int)->Tensor[{typ}, ?, ?]:
  cells:[{typ}]=[1,2,3,4]
  tensor(cells,[rows,4/rows])+1
fn consume(grid:Tensor[{typ}, ?, ?])->{typ}:
  total:{typ}=0
  for i in range(2):
    total=total+grid[i,1]
  total+2
fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    result=try await spawn make(2) on gpu
    value=try await spawn consume(result) on gpu
    int(value)
''', 10)
cases['device-mutual-recursion'] = ((fixtures/'device-mutual-recursion.k').read_text(), 1501)
cases['result-record-input-and-fresh-bool'] = ('''type Batch:
  values:[Bool]
fn collect(batch:Batch)->Batch:
  Batch {values:copy(batch.values)}
fn main()->int ! TaskError:
  input=Batch {values:[true,false]}
  try parallel:
    result=try await spawn collect(input) on devices.gpu()
    result.values[1]=true
    if result.values[0] and result.values[1] and not input.values[1]:42
    else:0
''',42)
cases['result-no-capture-paged-and-scalar'] = ('''type Batch:
  values:[int]
  count:int
fn collect(n:int)->Batch:
  Batch {values:generate(n,fn(i):i+2),count:n}
fn main()->int ! TaskError:
  try parallel:
    result=try await spawn collect(9000) on devices.gpu()
    result.values[8999]+result.count
''',18001)
cases['result-scalar-only'] = ('''type Pair:
  first:int8
  second:Bool
fn collect()->Pair:
  Pair {first:-128,second:true}
fn main()->int ! TaskError:
  try parallel:
    pair=try await spawn collect() on devices.gpu()
    if pair.second:int(pair.first)+128
    else:1
''',0)
known = set(cases) | {'reject-'+prefix+name for prefix in ('record-','gpu-record-')
                     for name in ('field','field-alias','field-sort')}
known |= {'reject-device-string-body','reject-device-host-only-record-result',
          'reject-closure-write', 'reject-closure-sort', 'reject-closure-string-capture'}
if set(a.case)-known:
    p.error('unknown case: '+', '.join(sorted(set(a.case)-known)))
results = []
for name,(source,expected) in cases.items():
    if a.case and name not in a.case:
        continue
    root = a.root/name
    root.mkdir(exist_ok=True)
    cpu = root/'cpu.k'
    if name == 'device-mutual-recursion':
        # Compare the identical recursive functions through the ordinary CPU call ABI.
        cpu.write_text(source[:source.index('fn main()')]+'fn main()->int:\n  even(1000)\n')
    else:
        cpu_source = source.replace(' on devices.gpu()', '').replace(' on devices.cpu()', '')
        if name.startswith(('device-tensor-resident-', 'device-tensor-return-', 'gpu-host-closure-resident-')):
            cpu_source = cpu_source.replace('    gpu=devices.gpu()\n', '').replace(' on gpu', '')
            cpu_source = cpu_source.replace('try await transfer(grid,gpu)', 'grid')
        cpu.write_text(cpu_source)
    reference = root/('cpu'+suffix)
    run([a.compiler,cpu,*flags,'-o',reference])
    assert int(run([reference])) == expected, (name,'CPU',expected)
    gpu = root/'gpu.k'
    gpu.write_text(source)
    bundle = root/'device'
    fallback = name == 'entry-float64' or (name in ('device-closure-float64','device-tensor-return-float64','gpu-host-closure-tensor-float64') and a.backend == 'metal')
    policy = ['--arithmetic','hardware-with-software-fallback'] if fallback else []
    run([a.compiler,gpu,'--target',a.backend,*flags,*policy,'-o',bundle])
    host = (bundle/'host.cpp').read_text()
    if name.startswith(('local-recursive-', 'device-mutual-recursion', 'device-recursive-', 'device-nested-closure', 'device-array-closure', 'device-closure-float64', 'device-tensor-')):
        assert 'NumericInstructionKind::DeviceControl' in host, name
    elif name.startswith("gpu-host-closure"):
        assert "kalandra_gpu_closure_tag_" in host and "DeviceGraphArgument<GpuClosureCapture" in host, name
        assert "NumericInstructionKind::DeviceControl" in host, name
    elif name == "host-closure":
        assert "NativeCallable" in host and "kalandra_gpu_callable_layout_" in host, name
        assert "HostArrayScopes" in host and "HostArrayWork" in host, name
    else:
        assert 'NativeRecord' in host and 'kalandra_gpu_record_layout_' in host, name
        assert 'HostArrayScopes' in host and 'HostArrayWork' in host, name
    if name in ('device-tensor-resident-mixed', 'device-tensor-resident-alias',
                'device-tensor-resident-graph'):
        assert 'DeviceResidentArgument<' in host, name
    if name.startswith('graph-entry-'):
        assert 'DeviceGraphArgument<' in host and 'NumericInstructionKind::DeviceControl' in host, name
    if name.startswith('graph-result-'):
        assert 'GpuGraphCapture' in host and 'DeviceAggregateResultJob<' in host, name
    assert any('gpu.func' in f.read_text() for f in bundle.glob('*.mlir')), name
    outputs = []
    if not a.compile_only:
        for workers in ('1','4'):
            out = run([bundle/('program'+suffix)],dict(os.environ,KALANDRA_WORKERS=workers))
            assert int(out,16) == expected, (name,workers,out,expected)
            outputs.append(out)
    results.append({'case':name,'expected':expected,'status':'compiled' if a.compile_only else 'executed','outputs':outputs})
    print(name,results[-1]['status'],flush=True)
for name,mutation in [(prefix+name,mutation) for prefix in ('record-','gpu-record-')
                      for name,mutation in [('field','batch.values[0]=9'),
                                            ('field-alias','alias[0]=9'),
                                            ('field-sort','sort(batch.values)')]]:
    if a.case and 'reject-'+name not in a.case:
        continue
    root=a.root/('reject-'+name)
    root.mkdir(exist_ok=True)
    source=root/'gpu.k'
    target = 'gpu' if name.startswith('gpu-') else 'cpu'
    source.write_text(common+work+'fn main()->int ! TaskError:\n  batch=make(1)\n  alias=batch.values\n  try parallel:\n    pending=spawn score(batch) on devices.'+target+'()\n    '+mutation+'\n    n=try await pending\n    try await spawn work(n) on devices.gpu()\n')
    r=subprocess.run(list(map(str,[a.compiler,source,'--target',a.backend,'-o',root/'device'])),capture_output=True,text=True,timeout=120)
    assert r.returncode!=0 and 'S077' in r.stderr,(name,r.returncode,r.stdout,r.stderr)
    results.append({'case':'reject-'+name,'status':'rejected','diagnostic':'S077'})
    print('reject-'+name,'rejected S077',flush=True)
for mutation in ('values[0]=99', 'sort(values)'):
    name = 'reject-closure-' + ('write' if mutation.startswith('values') else 'sort')
    if a.case and name not in a.case:
        continue
    root = a.root/name
    root.mkdir(exist_ok=True)
    path = root/'gpu.k'
    path.write_text('fn work(callback:fn(int)->int)->int:\n  callback(1)\n'
                    'fn main()->int ! TaskError:\n  values=[7,11]\n'
                    '  callback=fn(n:int):n+values[0]\n  try parallel:\n'
                    '    pending=spawn work(callback) on devices.gpu()\n    ' + mutation +
                    '\n    try await pending\n')
    r = subprocess.run(list(map(str, [a.compiler, path, '--target', a.backend, '-o', root/'device'])),
                       capture_output=True, text=True, timeout=120)
    assert r.returncode != 0 and 'S077' in r.stderr, (name, r.returncode, r.stdout, r.stderr)
    results.append({'case': name, 'status': 'rejected', 'diagnostic': 'S077'})
    print(name, 'rejected S077', flush=True)
for name,source in {
    'closure-string-capture':'fn make()->fn(int)->int:\n  text="hello"\n  fn(n:int):length(text)+n\nfn work(callback:fn(int)->int)->int:\n  callback(1)\nfn main()->int ! TaskError:\n  callback=make()\n  try parallel:\n    try await spawn work(callback) on devices.gpu()\n',
    'device-string-body':'fn work(n:int)->int:\n  message="text"\n  length(message)+n\nfn main()->int ! TaskError:\n  try parallel:\n    try await spawn work(1) on devices.gpu()\n',
    'device-host-only-record-result':'''type Message:
  text:String
fn device()->Message:
  Message {text:"hello"}
fn main()->int ! TaskError:
  try parallel:
    value=try await spawn device() on devices.gpu()
    length(value.text)
''',
}.items():
    if a.case and 'reject-'+name not in a.case:
        continue
    root=a.root/('reject-'+name)
    root.mkdir(exist_ok=True)
    path=root/'gpu.k'
    path.write_text(source)
    r=subprocess.run(list(map(str,[a.compiler,path,'--target',a.backend,'-o',root/'device'])),capture_output=True,text=True,timeout=120)
    assert r.returncode!=0 and 'G001' in r.stderr,(name,r.returncode,r.stdout,r.stderr)
    if name == 'closure-string-capture':
        assert "capture 'text'" in r.stderr and 'String' in r.stderr, r.stderr
    results.append({'case':'reject-'+name,'status':'rejected','diagnostic':'G001'})
    print('reject-'+name,'rejected G001',flush=True)
(a.root/'report.json').write_text(json.dumps({'complete':True,'fullCorpus':not bool(a.case),'selectedCases':a.case,'gpuExecution':not a.compile_only,
    'backend':a.backend,'variant':a.variant,'scope':'native host aggregates and typed whole-record GPU inputs and results',
    'results':results},indent=2)+'\n')
