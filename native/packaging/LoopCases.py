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
