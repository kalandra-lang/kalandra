"""Independent source/oracle cases for invocation-local device array storage."""

def cases():
    result = {}
    for typ in ('int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64', 'float32', 'float64'):
        result['typed-'+typ] = (f'''fn work(n:{typ})->{typ}:
  values:[{typ}]=[1,2,3]
  values[1]=n
  total:{typ}=0
  for value in values:
    total=total+value
  total
''', f'work({typ}(4))', typ, 8)
    result.update({
        'boolean-elements': ('''fn work(n:int)->int:
  flags=[true,false,true]
  flags[1]=n>0
  total=0
  for i in range(length(flags)):
    if flags[i]:
      total=total+i+1
  total
''', 'work(1)', 'int', 6),
        'assignment-alias': ('''fn work(n:int)->int:
  values=[1,2,3]
  alias=values
  alias[1]=n
  values[0]+values[1]+values[2]
''', 'work(9)', 'int', 13),
        'copy-slice-concat': ('''fn work(n:int)->int:
  original=[1,2,3]
  alias=original
  copied=copy(original)
  alias[0]=n
  copied[1]=10
  combined=concat(slice(original,1,3),copied)
  original[0]*100+copied[0]*10+combined[3]+length(combined)
''', 'work(8)', 'int', 825),
        'reclaim-dead-keep-alias': ('''fn work(n:int)->int:
  stable=[10,11,12]
  alias=stable
  i=0
  total=0
  while i<n:
    temp=[i+1,i+2]
    total=total+temp[0]
    i=i+1
  alias[1]=21
  total+stable[1]+stable[2]-33
''', 'work(1000)', 'int', 500500),
        'reassigned-array-root': ('''fn work(n:int)->int:
  values=[1]
  i=0
  while i<n:
    values=[i+1]
    i=i+1
  values[0]
''', 'work(100)', 'int', 100),
        'mutating-local-helper': ('''fn bump(mut values:[int],n:int)->int:
  values[0]=n
  values[1]
fn work(n:int)->int:
  values=[1,2]
  prior=bump(values,n)
  values[0]*10+prior
''', 'work(8)', 'int', 82),
        'internal-array-result': ('''fn make(n:int)->[int]:
  [n,n+1,n+2]
fn work(n:int)->int:
  values=make(n)
  total=0
  for value in values:
    total=total+value
  total
''', 'work(4)', 'int', 15),
        'nested-break-continue': ('''fn work(n:int)->int:
  values=[1,2,3,4]
  total=0
  for value in values:
    if value==2:
      continue
    for j in range(n):
      if j>2:
        break
      total=total+value+j
  total
''', 'work(5)', 'int', 33),
        'empty-arrays': ('''fn work(n:int)->int:
  values:[int]=[]
  copied=copy(values)
  combined=concat(copied,values)
  total=n
  for value in combined:
    total=total+value
  total+length(combined)
''', 'work(7)', 'int', 7),
        'branch-lengths': ('''fn work(n:int)->int:
  values=if n>0:
    [1,2,3]
  else:
    [4,5]
  total=0
  for value in values:
    total=total+value
  total
''', 'work(1)', 'int', 6),
        'empty-slice-concat': ('''fn work(n:int)->int:
  values=[1,2,3]
  empty=slice(values,n,n)
  copied=concat(empty,values)
  copied[2]+length(empty)
''', 'work(2)', 'int', 3),
        'word-boundary-bits': ('''fn work(n:int)->int:
  values:[int8]=[-128,127,-1]
  copied=copy(values)
  copied[2]=int8(n)
  int(values[0])+int(copied[1])+int(copied[2])
''', 'work(-2)', 'int', -3),
    })
    result['for-snapshot-rebinding'] = ("""fn work(n:int)->int:
  values=[1,2,3]
  total=0
  for value in values:
    values=[n]
    total=total+value
  total+values[0]
""", 'work(10)', 'int', 16)
    result['plain-record-array-gc'] = ("""type Row:
  value:int
  enabled:Bool
fn work(n:int)->int:
  row=Row {value:7,enabled:true}
  stable=[row.value,8,9]
  total=0
  for i in range(n):
    temp=[i,i+1]
    if row.enabled:
      total=total+temp[0]
  total+stable[0]+row.value-14
""", 'work(1000)', 'int', 499500)
    def ordered(values):
        value = 0
        for item in values:
            value = value * 13 + item
        return value
    result.update({
        'ordered-nested-yield': ("""fn work(n:int)->int:
  values=for i in range(n):
    if i%2==0:
      continue
    for j in range(3):
      if j==2:
        break
      yield i*10+j
  result=0
  for value in values:
    result=result*13+value
  result
""", 'work(10)', 'int', ordered([i*10+j for i in range(10) if i%2 for j in range(2)])),
        'empty-yield': ("""fn work(n:int)->int:
  values=for i in range(n):
    if i<0:
      yield i
  length(values)
""", 'work(10)', 'int', 0),
        'while-yield-break': ("""fn work(n:int)->int:
  i=0
  values=while i<n:
    i=i+1
    if i>4:
      break
    if i%2==1:
      yield i
  result=0
  for value in values:
    result=result*13+value
  result
""", 'work(20)', 'int', ordered([1,3])),
        'nested-collectors-gc': ("""fn work(n:int)->int:
  values=for i in range(n):
    inner=for j in range(3):
      yield i*10+j
    for value in inner:
      if value%2==0:
        yield value
  result=0
  for value in values:
    result=result*13+value
  result
""", 'work(4)', 'int', ordered([i*10+j for i in range(4) for j in range(3) if j%2==0])),
        'collector-growth': ("""fn work(n:int)->int:
  values=for i in range(n):
    yield i
  length(values)+values[n-1]+values[0]
""", 'work(1000)', 'int', 1999),
        'boolean-yield': ("""fn work(n:int)->int:
  flags=for i in range(n):
    yield i%2==0
  total=0
  for flag in flags:
    if flag:
      total=total+1
  total
""", 'work(6)', 'int', 3),
    })
    for typ in ('int8', 'int16', 'int32', 'int64', 'uint8', 'uint16', 'uint32', 'uint64', 'float32', 'float64'):
        result['map-'+typ] = (f"""fn work(n:{typ})->{typ}:
  values:[{typ}]=[1,2,3]
  changed=map(values,fn(value):value+n)
  total:{typ}=0
  for value in changed:
    total=total+value
  total+values[0]
""", f'work({typ}(4))', typ, 19)
    result['map-bool-output'] = ("""fn work(n:int)->int:
  flags=generate(n,fn(i):i%2==0)
  inverted=map(flags,fn(value):not value)
  total=0
  for i in range(length(inverted)):
    if inverted[i]:
      total=total+i+1
  total
""", 'work(6)', 'int64', 12)
    result['generate-map-captures'] = ("""fn work(n:int)->int:
  values=generate(n,fn(i):i*3+n)
  doubled=map(values,fn(value):value*2+1)
  total=0
  for value in doubled:
    total=total*7+value
  total+values[0]
""", 'work(5)', 'int64', 33612)
    result['generate-empty'] = ("""fn work(n:int)->int:
  values=generate(n,fn(i):1/(n-n))
  length(values)
""", 'work(0)', 'int64', 0)
    result['map-callback-array-gc'] = ("""fn transform(value:int)->int:
  temporary=[value,value+1]
  temporary[0]+temporary[1]
fn work(n:int)->int:
  values=generate(n,fn(i):i)
  changed=map(values,transform)
  total=0
  for value in changed:
    total=total+value
  total+values[n-1]
""", 'work(1000)', 'int64', 1000999)
    return result


def fault_cases():
    result = {}
    for typ, argument in (('int32', '-1'), ('int64', '-1'), ('uint32', '4294967295'), ('uint64', 'uint64(9223372036854775807)+uint64(1)')):
        result['bounds-'+typ] = (f'''fn work(n:{typ})->int:
  values=[1,2,3]
  values[n]
''', f'work({typ}({argument}))', 5)
    result['bounds-before-rhs'] = ('''fn work(n:int)->int:
  values=[1,2,3]
  values[n]=1/(n-n)
  values[0]
''', 'work(4)', 5)
    result['slice-negative'] = ('''fn work(n:int8)->int:
  values=[1,2,3]
  copied=slice(values,n,2)
  length(copied)
''', 'work(int8(-1))', 5)
    result['slice-reversed'] = ('''fn work(n:int)->int:
  values=[1,2,3]
  copied=slice(values,n,1)
  length(copied)
''', 'work(2)', 5)
    result['partial-yield-fault'] = ("""fn work(n:int)->int:
  values=for i in range(n):
    yield i+1
    if i==2:
      yield 1/(n-n)
  length(values)
""", 'work(5)', 4)
    result['generate-negative'] = ("""fn work(n:int)->int:
  values=generate(n,fn(i):1/(n-n))
  length(values)
""", 'work(-1)', 5)
    result['map-callback-bounds-first'] = ("""fn transform(value:int)->int:
  values=[1,2]
  if value==0:
    values[value+9]
  else:
    1/(value-value)
fn work(n:int)->int:
  values=generate(n,fn(i):i)
  result=map(values,transform)
  length(result)
""", 'work(4)', 5)
    result['map-callback-arithmetic'] = ("""fn work(n:int)->int:
  values=generate(n,fn(i):i)
  changed=map(values,fn(value):1/(value-value))
  length(changed)
""", 'work(4)', 4)
    return result
