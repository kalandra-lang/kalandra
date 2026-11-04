"""Shared executable contracts for statement/expression loops and lexical exits."""
CASES = {
    'while-break': ('''fn main()->int:
  i=0
  result=while i<10:
    i=i+1
    if i==2:
      continue
    if i==4:
      break i*10
  else:
    7
  result+i
''', '44'),
    'while-zero': ('''fn main()->String:
  while false:
    break "wrong"
  else:
    "zero"
''', 'zero'),
    'while-exhaustion': ('''fn main()->int:
  i=0
  while i<4:
    i=i+1
    continue
  else:
    i*10
''', '40'),
    'while-lazy-else': ('''fn zero()->int:
  0
fn main()->int:
  while true:
    break 42
  else:
    1/zero()
''', '42'),
    'for-exits': ('''fn main()->int:
  total=0
  for i in range(10):
    if i==2:
      continue
    if i==5:
      break
    total=total+i
  found=for value in [3,7,9]:
    if value>5:
      break value
  else:
    0
  total+found
''', '15'),
    'descending': ('''fn main()->int:
  total=0
  for i in range(5,-2,-2):
    total=total+i
  total
''', '8'),
    # INT64_MAX - 1 plus two overflows on the first advance.
    'range-positive-overflow': ('''fn main()->int:
  count=0
  for i in range(9223372036854775806,9223372036854775807,2):
    count=count+1
  count
''', '1'),
    'range-negative-overflow': ('''fn main()->int:
  count=0
  for i in range(-9223372036854775806,-9223372036854775808,-3):
    count=count+1
  count
''', '1'),
    'range-minimum-step': ('''fn main()->int:
  count=0
  for i in range(9223372036854775807,-9223372036854775808,-9223372036854775808):
    count=count+1
  count
''', '2'),
    'range-direction-empty': ('''fn main()->int:
  total=0
  for i in range(5,0):
    total=total+1
  for j in range(0,5,-1):
    total=total+1
  total
''', '0'),
    'nearest-loop': ('''fn main()->int:
  total=0
  for i in range(3):
    for j in range(10):
      if j==2:
        break
      total=total+1
    total=total+10
  total
''', '36'),
    'iterator-scope': ('''fn main()->int:
  i=42
  total=0
  for i in range(3):
    total=total+i
    i=100
  i+total
''', '45'),
    'array-once': ('''fn main()->int:
  values=[1,2,3]
  total=0
  for value in values:
    values=[9]
    total=total+value
  total+values[0]
''', '15'),
    'array-alias-result': ('''fn main()->int:
  values=[1,2]
  found=for item in [3,4]:
    break values
  else:
    [0]
  found[0]=9
  values[0]*10+length(found)
''', '92'),
    'array-empty-result': ('''fn main()->[int]:
  for value in [int(1)]:
    break [value,value+1]
  else:
    []
''', '[1, 2]'),
    'bool-array': ('''fn main()->int:
  count=0
  for flag in [true,false,true]:
    if not flag:
      continue
    count=count+1
  count
''', '2'),
    'owned-string-result': ('''fn main()->String:
  for text in ["a"+"b","unused"]:
    break text+"!"
  else:
    "empty"
''', 'ab!'),
    'record-result': ('''type Box:
  text:String
  values:[int]
fn main()->int:
  result=while true:
    break Box {text:"a"+"b",values:[7,8]}
  else:
    Box {text:"",values:[]}
  length(result.text)+result.values[0]
''', '9'),
    'callable-result': ('''fn main()->int:
  offset=40
  callback=for value in [2]:
    break fn(x:int):x+offset+value
  else:
    fn(x:int):x
  callback(0)
''', '42'),
    'callback-loop': ('''fn main()->[int]:
  generate(4,fn(index):
    for candidate in range(10):
      if candidate==index:
        break candidate*2
    else:
      -1
  )
''', '[0, 2, 4, 6]'),
    'group-exit': ('''fn identity(value:int)->int:
  value
fn main()->int ! TaskError:
  total=0
  for i in range(50):
    try parallel:
      pending=spawn identity(i)
      observed=try await pending
      total=total+observed
      if i<49:
        continue
      break
  total
''', '1225'),
    'range-evaluation-once': ('''fn next(mut state:[int])->int:
  state[0]=state[0]+1
  state[0]
fn main()->int:
  state=[0]
  total=0
  for value in range(next(state),next(state),next(state)):
    total=total+value
  state[0]*10+total
''', '31'),
    'empty-array-else': ('''fn main()->int:
  values:[int]=[]
  for value in values:
    break value
  else:
    42
''', '42'),
    'normal-effects-once': ('''fn main()->int:
  counter=0
  result=while false:
    break 42
  else:
    counter=counter+1
    7
  result+counter
''', '8'),
    'tensor-result': ('''fn main()->Tensor[int,?]:
  for value in [1]:
    break tensor([value,value+1])
  else:
    tensor([0])
''', 'tensor([1, 2], [2])'),

    'recursive-loop-function': ('''fn walk(n:int)->int:
  if n==0:
    0
  else:
    value=for index in range(1):
      break n
    else:
      0
    walk(n-1)+value
fn main()->int:
  walk(200)
''', '20100'),

    'expected-error-break': ('''error Stop:
  Done
fn read()->int ! Stop:
  fail Stop.Done
fn work()->int ! Stop:
  while true:
    break read()
  else:
    7
fn main()->int:
  work() catch fn(error):42
''', '42'),

    'literal-loop-result': ('''fn main()->int:
  while true:
    break 42
  else:
    0
''', '42'),

    'rank-zero-result': ('''fn main()->Tensor[int]:
  while true:
    break tensor(2)
  else:
    tensor(0)
''', 'tensor([2], [])'),
    'rank-eight-result': ('''fn main()->Tensor[int,1,1,1,1,1,1,1,?]:
  while true:
    break tensor([[[[[[[[1,2]]]]]]]])
  else:
    tensor([[[[[[[[0]]]]]]]])
''', 'tensor([1, 2], [1, 1, 1, 1, 1, 1, 1, 2])'),

}

CASES.update({
    'record-items': ('''type Box:
  value:int
fn main()->int:
  total=0
  for box in [Box {value:3},Box {value:7}]:
    total=total+box.value
  total
''', '10'),
    'function-items': ('''fn twice(x:int)->int:
  x*2
fn main()->int:
  total=0
  for callback in [twice,fn(x:int):x+1]:
    total=total+callback(3)
  total
''', '10'),
    'live-array': ('''fn main()->int:
  values=[1,2,3]
  total=0
  for item in values:
    values[2]=9
    total=total+item
  total
''', '12'),
    'statement-else': ('''fn main()->int:
  total=0
  for i in range(3):
    total=total+i
  else:
    total=total+10
  while total<15:
    total=total+1
  else:
    total=total+20
  total
''', '35'),
    'boolean-loop-result': ('''fn main()->Bool:
  while true:
    break true
  else:
    false
''', 'true'),
})

for _numeric in ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64'):
    CASES['array-element-' + _numeric] = (f'''fn main()->{_numeric}:
  total:{_numeric}=0
  for value in [{_numeric}(1),{_numeric}(2),{_numeric}(3)]:
    if value=={_numeric}(2):
      continue
    total=total+value
  total
''', '4')


REJECTED = {
    'break-outside': 'fn main()->int:\n  break\n  0\n',
    'continue-outside': 'fn main()->int:\n  continue\n  0\n',
    'continue-value': 'fn main()->int:\n  while true:\n    continue 1\n  0\n',
    'break-without-result': 'fn main()->int:\n  while true:\n    break\n  else:\n    0\n',
    'break-statement-value': 'fn main()->int:\n  while true:\n    break 1\n  0\n',
    'no-normal-result': 'fn main()->int:\n  result=while false:\n    break 1\n  result\n',
    'different-results': 'fn main()->int:\n  result=while true:\n    break true\n  else:\n    1\n  0\n',
    'lambda-boundary': 'fn main()->int:\n  while true:\n    f=fn()->int:\n      break\n      0\n    break\n  0\n',
    'iteration-name-leak': 'fn main()->int:\n  for i in range(3):\n    i+1\n  i\n',
    'pending-break': 'fn work()->int:\n  1\nfn main()->int ! TaskError:\n  try parallel:\n    while true:\n      pending=spawn work()\n      break\n    0\n',
    'pending-continue': 'fn work()->int:\n  1\nfn main()->int ! TaskError:\n  try parallel:\n    for i in range(3):\n      pending=spawn work()\n      continue\n    0\n',
    'unreachable-await': 'fn work(values:[int])->int:\n  values[0]\nfn main()->int ! TaskError:\n  values=[1]\n  try parallel:\n    task=spawn work(values)\n    while true:\n      break\n      try await task\n    values[0]=2\n    try await task\n',
    'active-reader': 'fn work(values:[int])->int:\n  values[0]\nfn main()->int ! TaskError:\n  values=[1]\n  try parallel:\n    task=spawn work(values)\n    for i in range(3):\n      if i==1:\n        break\n    values[0]=2\n    try await task\n',
    'range-type': 'fn main()->int:\n  for i in range(1.5):\n    i+1\n  0\n',
    'range-arity': 'fn main()->int:\n  for i in range():\n    i+1\n  0\n',
    'readonly-break-result': 'fn edit(input:[int])->int:\n  result=for i in range(1):\n    break input\n  else:\n    [0]\n  result[0]=1\n  result[0]\nfn main()->int:\n  edit([0])\n',
    'readonly-break-binding': 'fn edit(input:[int])->int:\n  local=[0]\n  for i in range(3):\n    local=input\n    break\n  local[0]=1\n  local[0]\nfn main()->int:\n  edit([0])\n',

}

# Eager yield collections are also portable and cross-target contracts.
CASES.update({
    'yield-nested': ('''fn main()->[int]:
  results=for row in range(4):
    for value in range(row):
      if value%2==0:
        yield row*10+value
  results
''', '[10, 20, 30, 32]'),
    'yield-while': ('''fn main()->[int]:
  i=0
  while i<6:
    i=i+1
    if i==2:
      continue
    yield i
    if i==4:
      break
    yield i*10
''', '[1, 10, 3, 30, 4]'),
    'yield-empty': ('''fn main()->[float32]:
  for i in range(0):
    yield 1
''', '[]'),
    'yield-filter-empty': ('''fn main()->[int]:
  for i in range(5):
    if i<0:
      yield i
''', '[]'),
    'yield-nested-exit': ('''fn main()->[int]:
  for i in range(3):
    for j in range(4):
      if j==2:
        break
      yield i*10+j
    yield 99
''', '[0, 1, 99, 10, 11, 99, 20, 21, 99]'),
    'yield-independent': ('''fn main()->[int]:
  for i in range(3):
    inner=for j in range(i):
      yield j
    yield length(inner)
''', '[0, 1, 2]'),
    'yield-string': ('''fn main()->String:
  words=for i in range(40):
    yield "été λ"+"!"
  words[39]
''', 'été λ!'),
    'yield-record': ('''type Box:
  value:int
  text:String
fn main()->int:
  boxes=for i in range(40):
    yield Box {value:i,text:"hello"+"!"}
  boxes[39].value+length(boxes[0].text)
''', '45'),
    'yield-functions': ('''fn main()->int:
  callbacks=for i in range(20):
    yield fn(x:int):x+i
  callbacks[0](1)+callbacks[19](1)
''', '21'),
    'yield-bool': ('''fn main()->int:
  flags=for i in range(40):
    yield i%2==0
  flags[0]=false
  if flags[0] or flags[39]:0
  else:length(flags)
''', '40'),
    'yield-mutable-result': ('''fn main()->[int]:
  values=for i in range(20):
    yield i
  values[19]=42
  slice(values,17,20)
''', '[17, 18, 42]'),
    'yield-range-evaluated-once': ('''fn main()->int:
  n=3
  values=for i in range(n):
    n=0
    yield i
  length(values)+n
''', '3'),
    'yield-grow': ('''fn main()->int:
  values=for i in range(1025):
    yield i
  values[1024]+length(values)
''', '2049'),
    'yield-lambda-local': ('''fn main()->int:
  results=for i in range(3):
    callback=fn(n:int)->[int]:
      for j in range(n):
        yield j
    yield length(callback(i))
  results[2]
''', '2'),
    'yield-task': ('''fn identity(x:int)->int:
  x
fn main()->int ! TaskError:
  try parallel:
    values=for i in range(4):
      child=spawn identity(i)
      yield try await child
    length(values)+values[3]
''', '7'),
    'yield-record-alias': ('''type Box:
  values:[int]
fn main()->int:
  source=[1]
  boxes=for i in range(20):
    yield Box {values:source}
  source[0]=7
  boxes[19].values[0]
''', '7'),
    'yield-int8': ('''fn main()->int8:
  values:[int8]=for i in range(3):
    yield int8(i+1)
  total:int8=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-uint8': ('''fn main()->uint8:
  values:[uint8]=for i in range(3):
    yield uint8(i+1)
  total:uint8=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-int16': ('''fn main()->int16:
  values:[int16]=for i in range(3):
    yield int16(i+1)
  total:int16=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-uint16': ('''fn main()->uint16:
  values:[uint16]=for i in range(3):
    yield uint16(i+1)
  total:uint16=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-int32': ('''fn main()->int32:
  values:[int32]=for i in range(3):
    yield int32(i+1)
  total:int32=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-uint32': ('''fn main()->uint32:
  values:[uint32]=for i in range(3):
    yield uint32(i+1)
  total:uint32=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-int64': ('''fn main()->int64:
  values:[int64]=for i in range(3):
    yield int64(i+1)
  total:int64=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-uint64': ('''fn main()->uint64:
  values:[uint64]=for i in range(3):
    yield uint64(i+1)
  total:uint64=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-float32': ('''fn main()->float32:
  values:[float32]=for i in range(3):
    yield float32(i+1)
  total:float32=0
  for value in values:
    total=total+value
  total
''', '6'),
    'yield-float64': ('''fn main()->float64:
  values:[float64]=for i in range(3):
    yield float64(i+1)
  total:float64=0
  for value in values:
    total=total+value
  total
''', '6'),
})

REJECTED.update({
    'yield-outside': '''fn main()->int:
  yield 1
  0
''',
    'yield-mixed-types': '''fn main()->[int]:
  for i in range(3):
    yield i
    yield true
''',
    'yield-break-value': '''fn main()->[int]:
  for i in range(3):
    yield i
    break 9
''',
    'yield-else-result': '''fn main()->[int]:
  for i in range(3):
    yield i
  else:
    [0]
''',
    'yield-array-element': '''fn main()->int:
  values=for i in range(3):
    yield [i]
  0
''',
    'yield-tensor-element': '''fn main()->int:
  values=for i in range(3):
    yield tensor([i])
  0
''',
    'yield-pending': '''fn work()->int:
  1
fn main()->int ! TaskError:
  try parallel:
    values=for i in range(3):
      pending=spawn work()
      yield i
    length(values)
''',
    'yield-task-value': '''fn work()->int:
  1
fn main()->int ! TaskError:
  try parallel:
    values=for i in range(3):
      yield spawn work()
    length(values)
''',
    'yield-function-boundary': '''fn main()->[int]:
  for i in range(3):
    callback=fn()->int:
      yield i
      0
    yield callback()
''',
    'yield-readonly-record': '''type Box:
  values:[int]
fn edit(input:[int])->int:
  boxes=for i in range(1):
    yield Box {values:input}
  boxes[0].values[0]=7
  0
fn main()->int:
  edit([1])
''',
    'yield-active-reader': '''type Box:
  values:[int]
fn read(boxes:[Box])->int:
  boxes[0].values[0]
fn main()->int ! TaskError:
  try parallel:
    source=[1]
    boxes=for i in range(1):
      yield Box {values:source}
    child=spawn read(boxes)
    source[0]=9
    try await child
''',
})

CASES.update({
    'yield-expected-error': ('''error Stop:
  Done
fn work()->[int] ! Stop:
  for i in range(20):
    if i==17:
      fail Stop.Done
    yield i
fn main()->int:
  values=work() catch fn(error):[42]
  values[0]
''', '42'),
    'yield-task-functions': ('''fn read(callbacks:[fn(int)->int])->int:
  callbacks[0](2)+callbacks[19](2)
fn main()->int ! TaskError:
  callbacks=for i in range(20):
    yield fn(x:int):x+i
  try parallel:
    child=spawn read(callbacks)
    try await child
''', '23'),
})

CASES['yield-helper-caller'] = ('''fn values()->[int]:
  for i in range(5):
    yield i
fn main()->int:
  a=values()
  a[4]+length(a)
''', '9')

CASES['yield-group-recovery'] = ('''fn divide(value:int)->int:
  6/value
fn main()->int ! TaskError:
  total=0
  for round in range(5):
    value=parallel:
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    recovered=value catch fn(error):7
    total=total+recovered
  total
''', '35')

CASES['yield-nested-recovery'] = ('''fn divide(value:int)->int:
  6/value
fn main()->[int] ! TaskError:
  for row in range(3):
    outcome=parallel:
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    value=outcome catch fn(error):7
    yield value
''', '[7, 7, 7]')

CASES['yield-union-primary-recovery'] = ('''error Stop:
  Done
fn checked(value:int)->int ! Stop:
  if value<0:
    fail Stop.Done
  else:
    value
fn divide(value:int)->int:
  6/value
fn main()->int ! TaskError:
  total=0
  for round in range(5):
    value=parallel:
      checkedValue=try checked(1)
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    recovered=value catch fn(error):
      match error:
        TaskError.Fault:7
        _:100
    total=total+recovered
  total
''', '35')
