"""Source Bool boundary cases with independent expected results and CPU counterparts."""
def cases():
    result = []
    def scalar(name, helper, call, expected, typ='bool'):
        cpu = helper+f'fn main()->{typ}:\n  return {call}\n'
        gpu = helper+f'''fn main()->{typ} ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    try await spawn {call} on gpu
'''
        result.append((name, cpu, gpu, expected, typ))
    scalar('identity-true', 'fn work(flag:bool)->bool:\n  return flag\n', 'work(true)', True)
    scalar('identity-false', 'fn work(flag:bool)->bool:\n  return flag\n', 'work(false)', False)
    helper = '''fn work(flag:bool,n:i64)->bool:
  result=flag
  i=0
  while i<n:
    result=not result
    i=i+1
  return result
'''
    scalar('loop-false', helper, 'work(true,17)', False)
    scalar('loop-true', helper, 'work(true,18)', True)
    scalar('record-bool', '''type Status:
  ready:bool
fn work(flag:bool)->bool:
  status=Status {ready:flag}
  return status.ready
''', 'work(true)', True)
    scalar('array-bool', '''fn work(n:i64)->bool:
  values=generate(n,fn(i):return i%2==0)
  alias=values
  sort(values)
  copied=copy(values)
  copied[0]=true
  return not alias[0] and copied[0] and values[n-1]
''', 'work(10)', True)
    scalar('bool-input-numeric-output', '''fn work(flag:bool,n:i64)->i64:
  return if flag:
    n
  else:
    0
''', 'work(true,42)', 42, 'i64')
    scalar('bool-input-false-numeric-output', '''fn work(flag:bool,n:i64)->i64:
  return if flag:
    n
  else:
    0
''', 'work(false,42)', 0, 'i64')
    gpu="""fn invert(flag:bool)->bool:
  return not flag
fn main()->bool ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    first=try await spawn invert(true) on gpu
    second=try await spawn invert(first) on devices.cpu()
    try await spawn invert(second) on gpu
"""
    cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
    result.append(('bool-round-trip',cpu,gpu,False,'bool'))
    predicate = 'fn predicate(flag:bool,n:i64)->bool:\n  return flag and n%2==1\n'
    for name, tasks in [('cached-await', '''    task=spawn predicate(true,7) on gpu
    first=try await task
    second=try await task
    first and second
'''), ('cpu-and-gpu', '''    first=try await spawn predicate(true,7) on gpu
    second=try await spawn predicate(true,7) on devices.cpu()
    first and second
''')]:
        gpu=predicate+'''fn main()->bool ! TaskError:
  try parallel:
    gpu=devices.gpu()
'''+tasks
        cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
        result.append((name,cpu,gpu,True,'bool'))
    gpu=predicate+'''fn outer(flag:bool,n:i64)->bool ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    first=try await spawn predicate(flag,n) on gpu
    second=try await spawn predicate(flag,n) on devices.cpu()
    first and second
fn main()->bool ! TaskError:
  return try parallel:
    try await spawn outer(true,7)
'''
    cpu=gpu.replace('    gpu=devices.gpu()\n','').replace(' on gpu','').replace(' on devices.cpu()','')
    result.append(('nested-task-bool',cpu,gpu,True,'bool'))
    helper='''fn positive(values:Tensor[f32,?])->bool:
  return reduceSum(values)>f32(0)
'''
    for name, values, expected in [('tensor-positive','1,2,3',True),('tensor-negative','-1,-2,-3',False)]:
        cpu=helper+f'fn main()->bool:\n  return positive(tensor([{values}]))\n'
        gpu=helper+f'''fn main()->bool ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    values:Tensor[f32,3]=tensor([{values}])
    resident=try await transfer(values,gpu)
    try await spawn positive(resident) on gpu
'''
        result.append((name,cpu,gpu,expected,'bool'))
    scalar('fault-before-bool-result', '''fn work(n:i64)->bool:
  value=1/(n-n)
  return value>0
''', 'work(4)', None)
    return result
