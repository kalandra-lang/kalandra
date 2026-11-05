"""Source Bool boundary cases with independent expected results and CPU counterparts."""
def cases():
    result = []
    def scalar(name, helper, call, expected, typ='Bool'):
        cpu = helper+f'fn main()->{typ}:\n  {call}\n'
        gpu = helper+f'''fn main()->{typ} ! TaskError:
  try parallel:
    gpu=devices.gpu()
    try await spawn {call} on gpu
'''
        result.append((name, cpu, gpu, expected, typ))
    scalar('identity-true', 'fn work(flag:Bool)->Bool:\n  flag\n', 'work(true)', True)
    scalar('identity-false', 'fn work(flag:Bool)->Bool:\n  flag\n', 'work(false)', False)
    helper = '''fn work(flag:Bool,n:int)->Bool:
  result=flag
  i=0
  while i<n:
    result=not result
    i=i+1
  result
'''
    scalar('loop-false', helper, 'work(true,17)', False)
    scalar('loop-true', helper, 'work(true,18)', True)
    scalar('record-bool', '''type Status:
  ready:Bool
fn work(flag:Bool)->Bool:
  status=Status {ready:flag}
  status.ready
''', 'work(true)', True)
    scalar('array-bool', '''fn work(n:int)->Bool:
  values=generate(n,fn(i):i%2==0)
  alias=values
  sort(values)
  copied=copy(values)
  copied[0]=true
  not alias[0] and copied[0] and values[n-1]
''', 'work(10)', True)
    scalar('bool-input-numeric-output', '''fn work(flag:Bool,n:int)->int:
  if flag:
    n
  else:
    0
''', 'work(true,42)', 42, 'int')
    scalar('bool-input-false-numeric-output', '''fn work(flag:Bool,n:int)->int:
  if flag:
    n
  else:
    0
''', 'work(false,42)', 0, 'int')
    gpu="""fn invert(flag:Bool)->Bool:
  not flag
fn main()->Bool ! TaskError:
  try parallel:
    gpu=devices.gpu()
    first=try await spawn invert(true) on gpu
    second=try await spawn invert(first) on devices.cpu()
    try await spawn invert(second) on gpu
"""
    cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
    result.append(('bool-round-trip',cpu,gpu,False,'Bool'))
    predicate = 'fn predicate(flag:Bool,n:int)->Bool:\n  flag and n%2==1\n'
    for name, tasks in [('cached-await', '''    task=spawn predicate(true,7) on gpu
    first=try await task
    second=try await task
    first and second
'''), ('cpu-and-gpu', '''    first=try await spawn predicate(true,7) on gpu
    second=try await spawn predicate(true,7) on devices.cpu()
    first and second
''')]:
        gpu=predicate+'''fn main()->Bool ! TaskError:
  try parallel:
    gpu=devices.gpu()
'''+tasks
        cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
        result.append((name,cpu,gpu,True,'Bool'))
    gpu=predicate+'''fn outer(flag:Bool,n:int)->Bool ! TaskError:
  try parallel:
    gpu=devices.gpu()
    first=try await spawn predicate(flag,n) on gpu
    second=try await spawn predicate(flag,n) on devices.cpu()
    first and second
fn main()->Bool ! TaskError:
  try parallel:
    try await spawn outer(true,7)
'''
    cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
    result.append(('nested-task-bool',cpu,gpu,True,'Bool'))
    helper='''fn positive(values:Tensor[float32,?])->Bool:
  reduceSum(values)>float32(0)
'''
    for name, values, expected in [('tensor-positive','1,2,3',True),('tensor-negative','-1,-2,-3',False)]:
        cpu=helper+f'fn main()->Bool:\n  positive(tensor([{values}]))\n'
        gpu=helper+f'''fn main()->Bool ! TaskError:
  try parallel:
    gpu=devices.gpu()
    values:Tensor[float32,3]=tensor([{values}])
    resident=try await transfer(values,gpu)
    try await spawn positive(resident) on gpu
'''
        result.append((name,cpu,gpu,expected,'Bool'))
    scalar('fault-before-bool-result', '''fn work(n:int)->Bool:
  value=1/(n-n)
  value>0
''', 'work(4)', None)
    return result
