"""Shared executable contracts for statement/expression loops and lexical exits."""
CASES = {
    'match-loop-exits': ('''enum Action:
  Skip
  End
  Keep
fn main()->i64:
  total=0
  for value in range(7):
    action=if value==2:
      Action.Skip
    else:
      if value==5:
        Action.End
      else:
        Action.Keep
    match action:
      Action.Skip:
        continue
      Action.End: break
      Action.Keep:
        total=total+value
        ()
  return total
''', '8'),
    'while-break': ('''fn main()->i64:
  i=0
  result=while i<10:
    i=i+1
    if i==2:
      continue
    if i==4:
      break i*10
  else:
    7
  return result+i
''', '44'),
    'while-zero': ('''fn main()->String:
  return while false:
    break "wrong"
  else:
    "zero"
''', 'zero'),
    'while-exhaustion': ('''fn main()->i64:
  i=0
  return while i<4:
    i=i+1
    continue
  else:
    i*10
''', '40'),
    'while-lazy-else': ('''fn zero()->i64:
  return 0
fn main()->i64:
  return while true:
    break 42
  else:
    1/zero()
''', '42'),
    'for-exits': ('''fn main()->i64:
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
  return total+found
''', '15'),
    'descending': ('''fn main()->i64:
  total=0
  for i in range(5,-2,-2):
    total=total+i
  return total
''', '8'),
    # INT64_MAX - 1 plus two overflows on the first advance.
    'range-positive-overflow': ('''fn main()->i64:
  count=0
  for i in range(9223372036854775806,9223372036854775807,2):
    count=count+1
  return count
''', '1'),
    'range-negative-overflow': ('''fn main()->i64:
  count=0
  for i in range(-9223372036854775806,-9223372036854775808,-3):
    count=count+1
  return count
''', '1'),
    'range-minimum-step': ('''fn main()->i64:
  count=0
  for i in range(9223372036854775807,-9223372036854775808,-9223372036854775808):
    count=count+1
  return count
''', '2'),
    'range-direction-empty': ('''fn main()->i64:
  total=0
  for i in range(5,0):
    total=total+1
  for j in range(0,5,-1):
    total=total+1
  return total
''', '0'),
    'nearest-loop': ('''fn main()->i64:
  total=0
  for i in range(3):
    for j in range(10):
      if j==2:
        break
      total=total+1
    total=total+10
  return total
''', '36'),
    'iterator-scope': ('''fn main()->i64:
  i=42
  total=0
  for i in range(3):
    total=total+i
    i=100
  return i+total
''', '45'),
    'array-once': ('''fn main()->i64:
  values=[1,2,3]
  total=0
  for value in values:
    values=[9]
    total=total+value
  return total+values[0]
''', '15'),
    'array-alias-result': ('''fn main()->i64:
  values=[1,2]
  found=for item in [3,4]:
    break values
  else:
    [0]
  found[0]=9
  return values[0]*10+length(found)
''', '92'),
    'array-empty-result': ('''fn main()->[i64]:
  return for value in [i64(1)]:
    break [value,value+1]
  else:
    []
''', '[1, 2]'),
    'bool-array': ('''fn main()->i64:
  count=0
  for flag in [true,false,true]:
    if not flag:
      continue
    count=count+1
  return count
''', '2'),
    'owned-string-result': ('''fn main()->String:
  return for text in ["a"+"b","unused"]:
    break text+"!"
  else:
    "empty"
''', 'ab!'),
    'record-result': ('''type Box:
  text:String
  values:[i64]
fn main()->i64:
  result=while true:
    break Box {text:"a"+"b",values:[7,8]}
  else:
    Box {text:"",values:[]}
  return length(result.text)+result.values[0]
''', '9'),
    'callable-result': ('''fn main()->i64:
  offset=40
  callback=for value in [2]:
    break fn(x:i64):return x+offset+value
  else:
    fn(x:i64):return x
  return callback(0)
''', '42'),
    'callback-loop': ('''fn main()->[i64]:
  return generate(4,fn(index):
    return for candidate in range(10):
      if candidate==index:
        break candidate*2
    else:
      -1
  )
''', '[0, 2, 4, 6]'),
    'group-exit': ('''fn identity(value:i64)->i64:
  return value
fn main()->i64 ! TaskError:
  total=0
  for i in range(50):
    try parallel:
      pending=spawn identity(i)
      observed=try await pending
      total=total+observed
      if i<49:
        continue
      break
  return total
''', '1225'),
    'range-evaluation-once': ('''fn next(mut state:[i64])->i64:
  state[0]=state[0]+1
  return state[0]
fn main()->i64:
  state=[0]
  total=0
  for value in range(next(state),next(state),next(state)):
    total=total+value
  return state[0]*10+total
''', '31'),
    'empty-array-else': ('''fn main()->i64:
  values:[i64]=[]
  return for value in values:
    break value
  else:
    42
''', '42'),
    'normal-effects-once': ('''fn main()->i64:
  counter=0
  result=while false:
    break 42
  else:
    counter=counter+1
    7
  return result+counter
''', '8'),
    'tensor-result': ('''fn main()->Tensor[i64,?]:
  return for value in [1]:
    break tensor([value,value+1])
  else:
    tensor([0])
''', 'tensor([1, 2], [2])'),

    'recursive-loop-function': ('''fn walk(n:i64)->i64:
  return if n==0:
    0
  else:
    value=for index in range(1):
      break n
    else:
      0
    walk(n-1)+value
fn main()->i64:
  return walk(200)
''', '20100'),

    'expected-error-break': ('''error Stop:
  Done
fn read()->i64 ! Stop:
  fail Stop.Done
fn work()->i64 ! Stop:
  return while true:
    break read()
  else:
    7
fn main()->i64:
  return work() catch fn(error):return 42
''', '42'),

    'literal-loop-result': ('''fn main()->i64:
  return while true:
    break 42
  else:
    0
''', '42'),

    'rank-zero-result': ('''fn main()->Tensor[i64]:
  return while true:
    break tensor(2)
  else:
    tensor(0)
''', 'tensor([2], [])'),
    'rank-eight-result': ('''fn main()->Tensor[i64,1,1,1,1,1,1,1,?]:
  return while true:
    break tensor([[[[[[[[1,2]]]]]]]])
  else:
    tensor([[[[[[[[0]]]]]]]])
''', 'tensor([1, 2], [1, 1, 1, 1, 1, 1, 1, 2])'),

}

CASES.update({
    'record-items': ('''type Box:
  value:i64
fn main()->i64:
  total=0
  for box in [Box {value:3},Box {value:7}]:
    total=total+box.value
  return total
''', '10'),
    'function-items': ('''fn twice(x:i64)->i64:
  return x*2
fn main()->i64:
  total=0
  for callback in [twice,fn(x:i64):return x+1]:
    total=total+callback(3)
  return total
''', '10'),
    'live-array': ('''fn main()->i64:
  values=[1,2,3]
  total=0
  for item in values:
    values[2]=9
    total=total+item
  return total
''', '12'),
    'statement-else': ('''fn main()->i64:
  total=0
  for i in range(3):
    total=total+i
  else:
    total=total+10
  while total<15:
    total=total+1
  else:
    total=total+20
  return total
''', '35'),
    'boolean-loop-result': ('''fn main()->bool:
  return while true:
    break true
  else:
    false
''', 'true'),
})

for _numeric in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64', 'f32', 'f64'):
    CASES['array-element-' + _numeric] = (f'''fn main()->{_numeric}:
  total:{_numeric}=0
  for value in [{_numeric}(1),{_numeric}(2),{_numeric}(3)]:
    if value=={_numeric}(2):
      continue
    total=total+value
  return total
''', '4')


REJECTED = {
    'break-outside': 'fn main()->i64:\n  break\n  return 0\n',
    'continue-outside': 'fn main()->i64:\n  continue\n  return 0\n',
    'continue-value': 'fn main()->i64:\n  while true:\n    continue 1\n  0\n',
    'break-without-result': 'fn main()->i64:\n  return while true:\n    break\n  else:\n    0\n',
    'break-statement-value': 'fn main()->i64:\n  while true:\n    break 1\n  return 0\n',
    'no-normal-result': 'fn main()->i64:\n  result=while false:\n    break 1\n  result\n',
    'different-results': 'fn main()->i64:\n  result=while true:\n    break true\n  else:\n    1\n  return 0\n',
    'lambda-boundary': 'fn main()->i64:\n  while true:\n    f=fn()->i64:\n      break\n      return 0\n    break\n  return 0\n',
    'iteration-name-leak': 'fn main()->i64:\n  for i in range(3):\n    i+1\n  return i\n',
    'pending-break': 'fn work()->i64:\n  return 1\nfn main()->i64 ! TaskError:\n  return try parallel:\n    while true:\n      pending=spawn work()\n      break\n    0\n',
    'pending-continue': 'fn work()->i64:\n  return 1\nfn main()->i64 ! TaskError:\n  return try parallel:\n    for i in range(3):\n      pending=spawn work()\n      continue\n    0\n',
    'unreachable-await': 'fn work(values:[i64])->i64:\n  return values[0]\nfn main()->i64 ! TaskError:\n  values=[1]\n  return try parallel:\n    task=spawn work(values)\n    while true:\n      break\n      try await task\n    values[0]=2\n    try await task\n',
    'active-reader': 'fn work(values:[i64])->i64:\n  return values[0]\nfn main()->i64 ! TaskError:\n  values=[1]\n  return try parallel:\n    task=spawn work(values)\n    for i in range(3):\n      if i==1:\n        break\n    values[0]=2\n    try await task\n',
    'range-type': 'fn main()->i64:\n  for i in range(1.5):\n    i+1\n  return 0\n',
    'range-arity': 'fn main()->i64:\n  for i in range():\n    i+1\n  return 0\n',
    'readonly-break-result': 'fn edit(input:[i64])->i64:\n  result=for i in range(1):\n    break input\n  else:\n    [0]\n  result[0]=1\n  return result[0]\nfn main()->i64:\n  return edit([0])\n',
    'readonly-break-binding': 'fn edit(input:[i64])->i64:\n  local=[0]\n  for i in range(3):\n    local=input\n    break\n  local[0]=1\n  return local[0]\nfn main()->i64:\n  return edit([0])\n',

}

# Eager yield collections are also portable and cross-target contracts.
CASES.update({
    'yield-nested': ('''fn main()->[i64]:
  results=for row in range(4):
    for value in range(row):
      if value%2==0:
        yield row*10+value
  return results
''', '[10, 20, 30, 32]'),
    'yield-while': ('''fn main()->[i64]:
  i=0
  return while i<6:
    i=i+1
    if i==2:
      continue
    yield i
    if i==4:
      break
    yield i*10
''', '[1, 10, 3, 30, 4]'),
    'yield-empty': ('''fn main()->[f32]:
  return for i in range(0):
    yield 1
''', '[]'),
    'yield-filter-empty': ('''fn main()->[i64]:
  return for i in range(5):
    if i<0:
      yield i
''', '[]'),
    'yield-nested-exit': ('''fn main()->[i64]:
  return for i in range(3):
    for j in range(4):
      if j==2:
        break
      yield i*10+j
    yield 99
''', '[0, 1, 99, 10, 11, 99, 20, 21, 99]'),
    'yield-independent': ('''fn main()->[i64]:
  return for i in range(3):
    inner=for j in range(i):
      yield j
    yield length(inner)
''', '[0, 1, 2]'),
    'yield-string': ('''fn main()->String:
  words=for i in range(40):
    yield "été λ"+"!"
  return words[39]
''', 'été λ!'),
    'yield-record': ('''type Box:
  value:i64
  text:String
fn main()->i64:
  boxes=for i in range(40):
    yield Box {value:i,text:"hello"+"!"}
  return boxes[39].value+length(boxes[0].text)
''', '45'),
    'yield-functions': ('''fn main()->i64:
  callbacks=for i in range(20):
    yield fn(x:i64):return x+i
  return callbacks[0](1)+callbacks[19](1)
''', '21'),
    'yield-bool': ('''fn main()->i64:
  flags=for i in range(40):
    yield i%2==0
  flags[0]=false
  return if flags[0] or flags[39]:0
  else:length(flags)
''', '40'),
    'yield-mutable-result': ('''fn main()->[i64]:
  values=for i in range(20):
    yield i
  values[19]=42
  return slice(values,17,20)
''', '[17, 18, 42]'),
    'yield-range-evaluated-once': ('''fn main()->i64:
  n=3
  values=for i in range(n):
    n=0
    yield i
  return length(values)+n
''', '3'),
    'yield-grow': ('''fn main()->i64:
  values=for i in range(1025):
    yield i
  return values[1024]+length(values)
''', '2049'),
    'yield-lambda-local': ('''fn main()->i64:
  results=for i in range(3):
    callback=fn(n:i64)->[i64]:
      return for j in range(n):
        yield j
    yield length(callback(i))
  return results[2]
''', '2'),
    'yield-task': ('''fn identity(x:i64)->i64:
  return x
fn main()->i64 ! TaskError:
  return try parallel:
    values=for i in range(4):
      child=spawn identity(i)
      yield try await child
    length(values)+values[3]
''', '7'),
    'yield-record-alias': ('''type Box:
  values:[i64]
fn main()->i64:
  source=[1]
  boxes=for i in range(20):
    yield Box {values:source}
  source[0]=7
  return boxes[19].values[0]
''', '7'),
    'yield-int8': ('''fn main()->i8:
  values:[i8]=for i in range(3):
    yield i8(i+1)
  total:i8=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-uint8': ('''fn main()->u8:
  values:[u8]=for i in range(3):
    yield u8(i+1)
  total:u8=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-int16': ('''fn main()->i16:
  values:[i16]=for i in range(3):
    yield i16(i+1)
  total:i16=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-uint16': ('''fn main()->u16:
  values:[u16]=for i in range(3):
    yield u16(i+1)
  total:u16=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-int32': ('''fn main()->i32:
  values:[i32]=for i in range(3):
    yield i32(i+1)
  total:i32=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-uint32': ('''fn main()->u32:
  values:[u32]=for i in range(3):
    yield u32(i+1)
  total:u32=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-int64': ('''fn main()->i64:
  values:[i64]=for i in range(3):
    yield i64(i+1)
  total:i64=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-uint64': ('''fn main()->u64:
  values:[u64]=for i in range(3):
    yield u64(i+1)
  total:u64=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-float32': ('''fn main()->f32:
  values:[f32]=for i in range(3):
    yield f32(i+1)
  total:f32=0
  for value in values:
    total=total+value
  return total
''', '6'),
    'yield-float64': ('''fn main()->f64:
  values:[f64]=for i in range(3):
    yield f64(i+1)
  total:f64=0
  for value in values:
    total=total+value
  return total
''', '6'),
})

REJECTED.update({
    'yield-outside': '''fn main()->i64:
  yield 1
  return 0
''',
    'yield-mixed-types': '''fn main()->[i64]:
  return for i in range(3):
    yield i
    yield true
''',
    'yield-break-value': '''fn main()->[i64]:
  return for i in range(3):
    yield i
    break 9
''',
    'yield-else-result': '''fn main()->[i64]:
  for i in range(3):
    yield i
  else:
    [0]
''',
    'yield-array-element': '''fn main()->i64:
  values=for i in range(3):
    yield [i]
  return 0
''',
    'yield-tensor-element': '''fn main()->i64:
  values=for i in range(3):
    yield tensor([i])
  return 0
''',
    'yield-pending': '''fn work()->i64:
  return 1
fn main()->i64 ! TaskError:
  return try parallel:
    values=for i in range(3):
      pending=spawn work()
      yield i
    length(values)
''',
    'yield-task-value': '''fn work()->i64:
  return 1
fn main()->i64 ! TaskError:
  return try parallel:
    values=for i in range(3):
      yield spawn work()
    length(values)
''',
    'yield-function-boundary': '''fn main()->[i64]:
  return for i in range(3):
    callback=fn()->i64:
      yield i
      return 0
    yield callback()
''',
    'yield-readonly-record': '''type Box:
  values:[i64]
fn edit(input:[i64])->i64:
  boxes=for i in range(1):
    yield Box {values:input}
  boxes[0].values[0]=7
  return 0
fn main()->i64:
  return edit([1])
''',
    'yield-active-reader': '''type Box:
  values:[i64]
fn read(boxes:[Box])->i64:
  return boxes[0].values[0]
fn main()->i64 ! TaskError:
  return try parallel:
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
fn work()->[i64] ! Stop:
  return for i in range(20):
    if i==17:
      fail Stop.Done
    yield i
fn main()->i64:
  values=work() catch fn(error):return [42]
  return values[0]
''', '42'),
    'yield-task-functions': ('''fn read(callbacks:[fn(i64)->i64])->i64:
  return callbacks[0](2)+callbacks[19](2)
fn main()->i64 ! TaskError:
  callbacks=for i in range(20):
    yield fn(x:i64):return x+i
  return try parallel:
    child=spawn read(callbacks)
    try await child
''', '23'),
})

CASES['yield-helper-caller'] = ('''fn values()->[i64]:
  return for i in range(5):
    yield i
fn main()->i64:
  a=values()
  return a[4]+length(a)
''', '9')

CASES['yield-group-recovery'] = ('''fn divide(value:i64)->i64:
  return 6/value
fn main()->i64 ! TaskError:
  total=0
  for round in range(5):
    value=parallel:
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    recovered=value catch fn(error):return 7
    total=total+recovered
  return total
''', '35')

CASES['yield-nested-recovery'] = ('''fn divide(value:i64)->i64:
  return 6/value
fn main()->[i64] ! TaskError:
  return for row in range(3):
    outcome=parallel:
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    value=outcome catch fn(error):return 7
    yield value
''', '[7, 7, 7]')

CASES['yield-union-primary-recovery'] = ('''error Stop:
  Done
fn checked(value:i64)->i64 ! Stop:
  return if value<0:
    fail Stop.Done
  else:
    value
fn divide(value:i64)->i64:
  return 6/value
fn main()->i64 ! TaskError:
  total=0
  for round in range(5):
    value=parallel:
      checkedValue=try checked(1)
      values=for i in range(3):
        child=spawn divide(2-i)
        yield try await child
      length(values)
    recovered=value catch fn(error):
      return match error:
        TaskError.Fault:7
        _:100
    total=total+recovered
  return total
''', '35')
