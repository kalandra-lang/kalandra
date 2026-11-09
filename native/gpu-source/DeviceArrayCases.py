"""Independent source/oracle cases for invocation-local device array storage."""

def cases():
    result = {}
    for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'):
        result['typed-'+typ] = (f'''fn work(n:{typ})->{typ}:
  values:[{typ}]=[1,2,3]
  values[1]=n
  total:{typ}=0
  for value in values:
    total=total+value
  return total
''', f'work({typ}(4))', typ, 8)
    result.update({
        'boolean-elements': ('''fn work(n:i64)->i64:
  flags=[true,false,true]
  flags[1]=n>0
  total=0
  for i in range(length(flags)):
    if flags[i]:
      total=total+i+1
  return total
''', 'work(1)', 'i64', 6),
        'assignment-alias': ('''fn work(n:i64)->i64:
  values=[1,2,3]
  alias=values
  alias[1]=n
  return values[0]+values[1]+values[2]
''', 'work(9)', 'i64', 13),
        'copy-slice-concat': ('''fn work(n:i64)->i64:
  original=[1,2,3]
  alias=original
  copied=copy(original)
  alias[0]=n
  copied[1]=10
  combined=concat(slice(original,1,3),copied)
  return original[0]*100+copied[0]*10+combined[3]+length(combined)
''', 'work(8)', 'i64', 825),
        'reclaim-dead-keep-alias': ('''fn work(n:i64)->i64:
  stable=[10,11,12]
  alias=stable
  i=0
  total=0
  while i<n:
    temp=[i+1,i+2]
    total=total+temp[0]
    i=i+1
  alias[1]=21
  return total+stable[1]+stable[2]-33
''', 'work(1000)', 'i64', 500500),
        'reassigned-array-root': ('''fn work(n:i64)->i64:
  values=[1]
  i=0
  while i<n:
    values=[i+1]
    i=i+1
  return values[0]
''', 'work(100)', 'i64', 100),
        'mutating-local-helper': ('''fn bump(mut values:[i64],n:i64)->i64:
  values[0]=n
  return values[1]
fn work(n:i64)->i64:
  values=[1,2]
  prior=bump(values,n)
  return values[0]*10+prior
''', 'work(8)', 'i64', 82),
        'internal-array-result': ('''fn make(n:i64)->[i64]:
  return [n,n+1,n+2]
fn work(n:i64)->i64:
  values=make(n)
  total=0
  for value in values:
    total=total+value
  return total
''', 'work(4)', 'i64', 15),
        'nested-break-continue': ('''fn work(n:i64)->i64:
  values=[1,2,3,4]
  total=0
  for value in values:
    if value==2:
      continue
    for j in range(n):
      if j>2:
        break
      total=total+value+j
  return total
''', 'work(5)', 'i64', 33),
        'empty-arrays': ('''fn work(n:i64)->i64:
  values:[i64]=[]
  copied=copy(values)
  combined=concat(copied,values)
  total=n
  for value in combined:
    total=total+value
  return total+length(combined)
''', 'work(7)', 'i64', 7),
        'branch-lengths': ('''fn work(n:i64)->i64:
  values=if n>0:
    [1,2,3]
  else:
    [4,5]
  total=0
  for value in values:
    total=total+value
  return total
''', 'work(1)', 'i64', 6),
        'empty-slice-concat': ('''fn work(n:i64)->i64:
  values=[1,2,3]
  empty=slice(values,n,n)
  copied=concat(empty,values)
  return copied[2]+length(empty)
''', 'work(2)', 'i64', 3),
        'word-boundary-bits': ('''fn work(n:i64)->i64:
  values:[i8]=[-128,127,-1]
  copied=copy(values)
  copied[2]=i8(n)
  return i64(values[0])+i64(copied[1])+i64(copied[2])
''', 'work(-2)', 'i64', -3),
    })
    result['for-snapshot-rebinding'] = ("""fn work(n:i64)->i64:
  values=[1,2,3]
  total=0
  for value in values:
    values=[n]
    total=total+value
  return total+values[0]
""", 'work(10)', 'i64', 16)
    result['plain-record-array-gc'] = ("""type Row:
  value:i64
  enabled:bool
fn work(n:i64)->i64:
  row=Row {value:7,enabled:true}
  stable=[row.value,8,9]
  total=0
  for i in range(n):
    temp=[i,i+1]
    if row.enabled:
      total=total+temp[0]
  return total+stable[0]+row.value-14
""", 'work(1000)', 'i64', 499500)
    def ordered(values):
        value = 0
        for item in values:
            value = value * 13 + item
        return value
    result.update({
        'ordered-nested-yield': ("""fn work(n:i64)->i64:
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
  return result
""", 'work(10)', 'i64', ordered([i*10+j for i in range(10) if i%2 for j in range(2)])),
        'empty-yield': ("""fn work(n:i64)->i64:
  values=for i in range(n):
    if i<0:
      yield i
  return length(values)
""", 'work(10)', 'i64', 0),
        'while-yield-break': ("""fn work(n:i64)->i64:
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
  return result
""", 'work(20)', 'i64', ordered([1,3])),
        'nested-collectors-gc': ("""fn work(n:i64)->i64:
  values=for i in range(n):
    inner=for j in range(3):
      yield i*10+j
    for value in inner:
      if value%2==0:
        yield value
  result=0
  for value in values:
    result=result*13+value
  return result
""", 'work(4)', 'i64', ordered([i*10+j for i in range(4) for j in range(3) if j%2==0])),
        'collector-growth': ("""fn work(n:i64)->i64:
  values=for i in range(n):
    yield i
  return length(values)+values[n-1]+values[0]
""", 'work(1000)', 'i64', 1999),
        'boolean-yield': ("""fn work(n:i64)->i64:
  flags=for i in range(n):
    yield i%2==0
  total=0
  for flag in flags:
    if flag:
      total=total+1
  return total
""", 'work(6)', 'i64', 3),
    })
    for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'):
        result['map-'+typ] = (f"""fn work(n:{typ})->{typ}:
  values:[{typ}]=[1,2,3]
  changed=map(values,fn(value):return value+n)
  total:{typ}=0
  for value in changed:
    total=total+value
  return total+values[0]
""", f'work({typ}(4))', typ, 19)
    result['map-bool-output'] = ("""fn work(n:i64)->i64:
  flags=generate(n,fn(i):return i%2==0)
  inverted=map(flags,fn(value):return not value)
  total=0
  for i in range(length(inverted)):
    if inverted[i]:
      total=total+i+1
  return total
""", 'work(6)', 'i64', 12)
    result['generate-map-captures'] = ("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return i*3+n)
  doubled=map(values,fn(value):return value*2+1)
  total=0
  for value in doubled:
    total=total*7+value
  return total+values[0]
""", 'work(5)', 'i64', 33612)
    result['generate-empty'] = ("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return 1/(n-n))
  return length(values)
""", 'work(0)', 'i64', 0)
    result['map-callback-array-gc'] = ("""fn transform(value:i64)->i64:
  temporary=[value,value+1]
  return temporary[0]+temporary[1]
fn work(n:i64)->i64:
  values=generate(n,fn(i):return i)
  changed=map(values,transform)
  total=0
  for value in changed:
    total=total+value
  return total+values[n-1]
""", 'work(1000)', 'i64', 1000999)
    for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'):
        result['sort-'+typ] = (f"""fn work(n:{typ})->{typ}:
  values:[{typ}]=[3,1,2,1]
  alias=values
  copied=sorted(values)
  copied[0]=n
  sort(values)
  return if alias[0]=={typ}(1) and alias[1]=={typ}(1) and alias[2]=={typ}(2) and alias[3]=={typ}(3) and copied[0]==n and copied[1]=={typ}(1) and copied[3]=={typ}(3):
    {typ}(7)
  else:
    {typ}(0)
""", f'work({typ}(4))', typ, 7)
    result['sort-bool'] = ("""fn work(n:i64)->i64:
  values=[true,false,true,false]
  alias=values
  copied=sorted(values)
  copied[0]=true
  sort(values)
  return if not alias[0] and not alias[1] and alias[2] and alias[3] and copied[0] and not copied[1]:
    n
  else:
    0
""", 'work(9)', 'i64', 9)
    result['sort-empty-singleton-repeat'] = ("""fn work(n:i64)->i64:
  empty=generate(n,fn(i):return i)
  sort(empty)
  copied=sorted(empty)
  single=[42]
  sort(single)
  result=sorted(single)
  return result[0]+length(copied)
""", 'work(0)', 'i64', 42)
    result['sort-large-checkpointed'] = ("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return n-i)
  alias=values
  copied=sorted(values)
  original=values[0]
  sort(values)
  total=0
  for i in range(n):
    total=total+i*copied[i]
  return total+original+alias[0]
""", 'work(1000)', 'i64', 333334001)
    result['sort-in-callback'] = ("""fn transform(value:i64)->i64:
  values=[value+2,value,value+1]
  copied=sorted(values)
  return copied[0]
fn work(n:i64)->i64:
  values=generate(n,fn(i):return n-i)
  changed=map(values,transform)
  total=0
  for value in changed:
    total=total+value
  return total
""", 'work(50)', 'i64', 1275)
    result.update({
        'record-only-array-root': ("""type Bundle:
  values:[i64]
fn make(n:i64)->Bundle:
  return Bundle {values:[n,n+1]}
fn score(bundle:Bundle)->i64:
  return bundle.values[0]+bundle.values[1]
fn work(n:i64)->i64:
  bundle=make(n)
  for i in range(1000):
    temporary=[i,i+1]
    if temporary[0]<0:
      break
  return score(bundle)
""", 'work(20)', 'i64', 41),
        'nested-record-array-roots': ("""type Inner:
  values:[i64]
  flags:[bool]
type Outer:
  row:Inner
  weight:i64
fn work(n:i64)->i64:
  holder=Outer {row:Inner {values:[n,n+1],flags:[true,false]},weight:3}
  for i in range(1000):
    temporary=[i,i+1]
    if temporary[0]<0:
      break
  return if holder.row.flags[0]:
    holder.row.values[1]*holder.weight
  else:
    0
""", 'work(20)', 'i64', 63),
        'enum-array-roots': ("""enum Box:
  Empty
  Some:
    values:[i64]
fn score(box:Box)->i64:
  return match box:
    Box.Empty:0
    Box.Some {values}:values[0]
fn work(n:i64)->i64:
  total=0
  for i in range(n):
    box=if i%2==0:Box.Some {values:[i,i+1]}
    else:Box.Empty
    for j in range(4):
      temporary=[j,i]
      total=total+temporary[0]
    total=total+score(box)
  return total
""", 'work(80)', 'i64', 2040),
        'record-array-alias-update': ("""type Bundle:
  values:[i64]
fn work(n:i64)->i64:
  original=Bundle {values:[1,2]}
  alias=original
  view=alias.values
  view[0]=n
  changed=original with {values:copy(original.values)}
  independent=changed.values
  independent[1]=10
  return original.values[0]*100+alias.values[1]*10+changed.values[1]
""", 'work(7)', 'i64', 730),
        'record-shadow-keeps-outer-root': ("""type Bundle:
  values:[i64]
fn work(n:i64)->i64:
  bundle=Bundle {values:[0]}
  for i in range(n):
    bundle=Bundle {values:[i+1]}
  return bundle.values[0]
""", 'work(1000)', 'i64', 0),
    })
    for typ in ('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'):
        result['record-owned-'+typ] = (f"""type Holder:
  values:[{typ}]
fn work(n:{typ})->{typ}:
  holder=Holder {{values:[n]}}
  for i in range(80):
    temporary=[i,i+1]
    if temporary[0]<0:
      break
  return holder.values[0]
""", f'work({typ}(7))', typ, 7)
    result['record-captured-in-map'] = ("""type Config:
  values:[i64]
fn transform(config:Config,value:i64)->i64:
  temporary=[value,config.values[0]]
  return temporary[0]+temporary[1]
fn work(n:i64)->i64:
  config=Config {values:[n]}
  values=generate(100,fn(index):return index)
  changed=map(values,fn(value):return transform(config,value))
  total=0
  for value in changed:
    total=total+value
  return total
""", 'work(7)', 'i64', 5650)
    return result


def fault_cases():
    result = {}
    for typ, argument in (('i32', '-1'), ('i64', '-1'), ('u32', '4294967295'), ('u64', 'u64(9223372036854775807)+u64(1)')):
        result['bounds-'+typ] = (f'''fn work(n:{typ})->i64:
  values=[1,2,3]
  return values[n]
''', f'work({typ}({argument}))', 5)
    result['bounds-before-rhs'] = ('''fn work(n:i64)->i64:
  values=[1,2,3]
  values[n]=1/(n-n)
  return values[0]
''', 'work(4)', 5)
    result['slice-negative'] = ('''fn work(n:i8)->i64:
  values=[1,2,3]
  copied=slice(values,n,2)
  return length(copied)
''', 'work(i8(-1))', 5)
    result['slice-reversed'] = ('''fn work(n:i64)->i64:
  values=[1,2,3]
  copied=slice(values,n,1)
  return length(copied)
''', 'work(2)', 5)
    result['partial-yield-fault'] = ("""fn work(n:i64)->i64:
  values=for i in range(n):
    yield i+1
    if i==2:
      yield 1/(n-n)
  return length(values)
""", 'work(5)', 4)
    result['generate-negative'] = ("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return 1/(n-n))
  return length(values)
""", 'work(-1)', 5)
    result['map-callback-bounds-first'] = ("""fn transform(value:i64)->i64:
  values=[1,2]
  return if value==0:
    values[value+9]
  else:
    1/(value-value)
fn work(n:i64)->i64:
  values=generate(n,fn(i):return i)
  result=map(values,transform)
  return length(result)
""", 'work(4)', 5)
    result['map-callback-arithmetic'] = ("""fn work(n:i64)->i64:
  values=generate(n,fn(i):return i)
  changed=map(values,fn(value):return 1/(value-value))
  return length(changed)
""", 'work(4)', 4)
    return result


def record_lifetime_cases():
    """Record-held roots must survive/reclaim through actual task settlement."""
    prefix = "type Holder:\n  values:[i64]\n"
    return {
        'finite': prefix+"""fn work(n:i64)->i64:
  holder=Holder {values:[n]}
  total=0
  for i in range(n):
    temporary=[i+1,n]
    total=total+temporary[0]
  return total+holder.values[0]-n
""",
        'infinite': prefix+"""fn work(n:i64)->i64:
  holder=Holder {values:[n]}
  i=0
  while true:
    temporary=[i,n]
    if holder.values[0]<0:
      break
    i=temporary[0]+1
  return i
""",
        'fault': prefix+"""fn work(n:i64)->i64:
  holder=Holder {values:[n,1]}
  alias=holder.values
  alias[n]=1/(n-n)
  return n
""",
    }
