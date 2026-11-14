"""Shared source-level lazy generator correctness fixtures."""
prefix = 'from "std/iter" import Iterator,next,close\nfrom "std/option" import Option\n'
cases = {
    "fusion-direct-range-adapters": (
        """from "std/iter" import iter,range as steps,map as mapped,filter
fn main()->i64:
  total=0
  for value in mapped(filter(iter(steps(5)),fn(x:i64)->bool:return x%2==0),fn(x:i64)->i64:return x*x):
    total=total+value
  return total
""",
        20,
        "",
    ),
    "fusion-direct-factory-arguments": (
        """fn values(start:i64,count:i64)->Iterator[i64,never]:
  return for i in range(count):
    yield start+i
fn main()->i64:
  count=5
  total=0
  for value in values(2,count):
    count=1
    total=total+value
  return total*10+count
""",
        201,
        "",
    ),
    "fusion-direct-callback-capture": (
        """from "std/iter" import iter,map as mapped
fn main()->i64:
  data=[1,2,3]
  factor=3
  total=0
  for value in mapped(iter(data),fn(x:i64)->i64:return x*factor):
    factor=99
    total=total+value
  return total+factor
""",
        117,
        "",
    ),
    "fusion-direct-take-zero-bound": (
        """from "std/iter" import take
fn values(count:i64)->Iterator[i64,never]:
  return for i in range(1/count):
    yield i
fn main()->i64:
  total=0
  for value in take(values(0),0):
    total=total+value
  return total
""",
        0,
        "",
    ),
    "fusion-direct-nested-construction": (
        """fn values(offset:i64)->Iterator[i64,never]:
  return for i in range(2):
    yield offset+i
fn main()->i64:
  total=0
  for offset in range(3):
    for value in values(offset):
      total=total+value
  return total
""",
        9,
        "",
    ),
    "fusion-direct-valued-break": (
        """fn values(count:i64)->Iterator[i64,never]:
  return for i in range(count):
    yield i
fn main()->i64:
  return for value in values(3):
    if value==2:
      break 42
  else:0
""",
        42,
        "",
    ),
    "fusion-array-loan-released-before-write": (
        """fn main()->i64:
  data=[1,2,3]
  values=for i in range(3):
    yield data[i]
  total=0
  for value in values:
    total=total+value
  data[0]=9
  return total+data[0]
""",
        15,
        "",
    ),
    "fusion-static-callback-array-capture": (
        """from "std/iter" import iter,range,map as mapped
fn main()->i64:
  data=[3,5,7]
  transform=fn(i:i64)->i64:return data[i]
  values=mapped(iter(range(3)),transform)
  total=0
  for value in values:
    total=total+value
  return total
""",
        15,
        "",
    ),
    "fusion-control-assignment-break": (
        """fn main()->i64:
  values=for i in range(5):
    yield i
  total=10
  for value in values:
    total=if value==2:
      break
    else: total+value
  return total
""",
        11,
        "",
    ),
    "fusion-control-assignment-continue": (
        """fn main()->i64:
  values=for i in range(5):
    yield i
  total=10
  for value in values:
    total=if value==2:
      continue
    else: total+value
  return total
""",
        18,
        "",
    ),
    "fusion-control-transfer-in-break-value": (
        """fn main()->i64:
  values=for i in range(5):
    yield i
  found=for value in values:
    break if value<2:
      continue
    else: value
  else: 17
  return found
""",
        2,
        "",
    ),
    "fusion-control-transfer-in-condition": (
        """fn main()->i64:
  values=for i in range(5):
    while (if i==2:
      break
    else: true
    ):
      yield i
      break
  total=0
  for value in values:
    total=total+value
  return total
""",
        1,
        "",
    ),
    "fusion-region-around-io": (
        """from "std/io" import println
from "std/fmt" import format
fn emit(text:String)->unit:
  done=println(text) catch fn(error):return ()
  return ()
fn main()->i64:
  emit("before")
  seed=3
  values=for i in range(3):
    yield seed+i
  seed=100
  total=0
  for value in values:
    total=total+value
  emit(format("{}:{}",total,seed))
  return total+seed
""",
        112,
        "before\n12:100\n",
    ),
    "fusion-region-multiple-scalar-updates": (
        """from "std/io" import println
from "std/iter" import take
from "std/fmt" import format
fn emit(text:String)->unit:
  done=println(text) catch fn(error):return ()
  return ()
fn main()->i64:
  left=10
  right=20
  values=for i in range(30):
    yield i
  limited=take(values,3)
  for value in limited:
    left=left+value
    right=right-value
  emit(format("{}:{}",left,right))
  return left+right
""",
        30,
        "13:17\n",
    ),
    "fusion-multiple-regions": (
        """from "std/io" import println
fn emit()->unit:
  done=println("between") catch fn(error):return ()
  return ()
fn main()->i64:
  first=for i in range(3):
    yield i
  total=0
  for value in first:
    total=total+value
  emit()
  second=for i in range(4):
    yield i*2
  for value in second:
    total=total+value
  return total
""",
        15,
        "between\n",
    ),
    "fusion-readonly-array-source": (
        """fn main()->i64:
  data=[1,2,3,4,5,6]
  values=for i in range(length(data)):
    yield data[i]*2
  total=0
  for value in values:
    total=total+value
  return total
""",
        42,
        "",
    ),
    "fusion-readonly-array-adapters": (
        """from "std/iter" import iter,map as mapped,filter
fn main()->i64:
  data=[1,2,3,4,5,6]
  values=mapped(filter(iter(data),fn(x:i64)->bool:return x%2==0),
    fn(x:i64)->i64:return x*x)
  total=0
  for value in values:
    total=total+value
  return total
""",
        56,
        "",
    ),
    "fusion-f32-rounding": (
        """fn main()->i64:
  a:f32=1.00000011920928955078125
  b:f32=0.99999988079071044921875
  c:f32=-1
  values=for i in range(3):
    yield a*b+c
  total=0
  for value in values:
    if value==0:
      total=total+1
  return total
""",
        3,
        "",
    ),
    "fusion-f64-rounding": (
        """fn main()->i64:
  a:f64=1.0000000000000002
  b:f64=0.9999999999999998
  c:f64=-1
  values=for i in range(3):
    yield a*b+c
  total=0
  for value in values:
    if value==0:
      total=total+1
  return total
""",
        3,
        "",
    ),
    "fusion-valued-empty-exhaustion": (
        """fn main()->i64:
  values=for i in range(0):
    yield i
  found=for value in values:
    break value
  else: 17
  return found
""",
        17,
        "",
    ),
    "fusion-private-scalar-capture": (
        """fn main()->i64:
  seed=3
  values=for i in range(4):
    seed=seed+1
    yield seed+i
  seed=100
  total=0
  for value in values:
    total=total+value
  return total+seed
""",
        128,
        "",
    ),
    "fusion-continue-resumes-after-yield": (
        """fn main()->i64:
  current=0
  values=while current<4:
    yield current
    current=current+1
  total=0
  for value in values:
    if value==2:
      continue
    total=total+value
  return total
""",
        4,
        "",
    ),
    "fusion-break-does-not-resume": (
        """fn main()->i64:
  zero=0
  values=for i in range(3):
    yield i+7
    bad=10/zero
  total=0
  for value in values:
    total=value
    break
  return total
""",
        7,
        "",
    ),
    "fusion-nested-yields": (
        """fn main()->i64:
  values=for i in range(3):
    for j in range(4):
      if j%2==0:
        yield i*10+j
    yield i
  total=0
  for value in values:
    total=total+value
  return total
""",
        69,
        "",
    ),
    "fusion-range-adapters": (
        """from "std/iter" import iter,range,map as mapped,filter
fn main()->i64:
  offset=3
  values=mapped(filter(iter(range(10)),fn(x:i64)->bool:return x%2==0),
    fn(x:i64)->i64:return x*x+offset)
  offset=100
  total=0
  for value in values:
    total=total+value
  return total
""",
        135,
        "",
    ),
    "fusion-take-stops-before-next": (
        """from "std/iter" import take
fn main()->i64:
  zero=0
  values=for i in range(3):
    yield i+7
    bad=10/zero
  limited=take(values,1)
  total=0
  for value in limited:
    total=total+value
  return total
""",
        7,
        "",
    ),
    "fusion-take-zero-defers-bounds": (
        """from "std/iter" import take
fn main()->i64:
  zero=0
  values=for i in range(10/zero):
    yield i
  limited=take(values,0)
  total=0
  for value in limited:
    total=total+value
  return total
""",
        0,
        "",
    ),
    "fusion-valued-exhaustion": (
        """fn main()->i64:
  values=for i in range(3):
    yield i
  found=for value in values:
    if value==1:
      break 42
  else: 7
  return found
""",
        42,
        "",
    ),
    "deferred-source-expression": (
        """from "std/io" import println
fn emit(text:String)->unit:
  done=println(text) catch fn(error):return ()
  return ()
fn bound()->i64:
  emit("bounds")
  return 3
fn values()->Iterator[i64,never]:
  return for i in range(bound()):
    yield i
fn main()->i64:
  cursor=values()
  emit("created")
  for value in cursor:
    break
  return 0
""",
        0,
        "created\nbounds\n",
    ),
    "unit-items": (
        """fn main()->i64:
  cursor=for i in range(3):
    yield ()
  count=0
  for item in cursor:
    count=count+1
  return count
""",
        3,
        "",
    ),
    "array-item-outlives-close": (
        """fn main()->i64:
  cursor=for i in range(3):
    values=[i,i+1]
    yield values
  first=for values in cursor:
    break values
  else:[0,0]
  first[0]=9
  return first[0]+first[1]
""",
        10,
        "",
    ),
    "tensor-item-outlives-close": (
        """fn main()->i64:
  cursor=for i in range(3):
    yield tensor([f32(i+1)])
  first=for value in cursor:
    break value
  else:tensor([f32(0)])
  return i64(first[0])
""",
        1,
        "",
    ),
    "immediate-materialization": (
        """fn main()->i64:
  count=3
  values=array(for value in range(count):
    count=0
    yield value
  )
  return length(values)+count
""",
        6,
        "",
    ),
    "deep-adapter-chain": (
        """from "std/iter" import iter,map as mapItems,take
fn adapt(own source:Iterator[i64,never],depth:i64)->Iterator[i64,never]:
  if depth==0:
    return source
  return adapt(mapItems(source,fn(value:i64)->i64:return value+1),depth-1)
fn main()->i64:
  values=array(take(adapt(iter([1,2,3]),32),2))
  return values[0]+values[1]
""",
        67,
        "",
    ),
    "adapter-chain": (
        """from "std/iter" import iter,map as mapItems,filter,take
fn main()->i64:
  doubled=mapItems(iter([1,2,3,4,5,6]),fn(value:i64)->i64:return value*2)
  selected=filter(doubled,fn(value:i64)->bool:return value%3!=0)
  values=array(take(selected,2))
  total=0
  for value in values:
    total=total+value
  return total
""",
        6,
        "",
    ),
    "take-zero-no-advance": (
        """from "std/iter" import take
from "std/io" import println
fn emit()->unit:
  done=println("wrong") catch fn(error):return ()
  return ()
fn values()->Iterator[i64,never]:
  return for value in range(3):
    emit()
    yield value
fn main()->i64:
  result=array(take(values(),0))
  return length(result)
""",
        0,
        "",
    ),
    "adapter-error-union": (
        """from "std/iter" import map as mapItems
error InputError:
  Bad
error MappingError:
  Bad:
    message:String
fn values()->Iterator[i64,InputError]:
  return for value in range(4):
    if value==2:
      fail InputError.Bad
    yield value
fn checked(value:i64)->i64 ! MappingError:
  if value==1:
    fail MappingError.Bad {message:"er"+"ror"}
  return value*2
fn plain(value:i64)->i64 ! MappingError:
  return value*2
fn run(failMapping:bool)->i64:
  callback=if failMapping:checked
  else:plain
  result=array(mapItems(values(),callback)) catch fn(error):
    return match error:
      InputError.Bad:[-i64(1)]
      MappingError.Bad {message}:
        if message=="error":[-i64(2)]
        else:[-i64(1000)]
  return result[0]
fn main()->i64:
  return run(true)+run(false)
""",
        -3,
        "",
    ),
    "filter-error": (
        """from "std/iter" import iter,filter
error Problem:
  Bad
fn selected(value:i64)->bool ! Problem:
  if value==2:
    fail Problem.Bad
  return value>0
fn main()->i64:
  result=array(filter(iter([1,2,3]),selected)) catch fn(error):return [-i64(5)]
  return result[0]
""",
        -5,
        "",
    ),
    "materialize": (
        """fn main()->i64:
  cursor=for value in range(5):
    yield value
  values=array(cursor)
  total=0
  for value in values:
    total=total+value
  return total
""",
        10,
        "",
    ),
    "materialize-copy": (
        """fn main()->i64:
  cursor=for i in range(1):
    values=[i+1,2]
    other=array(values)
    values[0]=9
    yield other[0]
  total=0
  for value in cursor:
    total=total+value
  return total
""",
        1,
        "",
    ),
    "generic-materialize-error": (
        """error Problem:
  Bad:
    message:String
fn values()->Iterator[i64,Problem]:
  return for value in range(4):
    if value==2:
      fail Problem.Bad {message:"er"+"ror"}
    yield value
fn collect[T,E](own cursor:Iterator[T,E])->[T] ! E:
  return try array(cursor)
fn main()->i64:
  result=collect(values()) catch fn(error):
    return match error:
      Problem.Bad {message}:
        if message=="error":[-i64(7)]
        else:[-i64(1000)]
  return result[0]
""",
        -7,
        "",
    ),
    "generic-item-capability": (
        """trait Bump:
  fn bump(value:Self)->i64
impl Bump for i64:
  fn bump(value:Self)->i64:
    return value+1
fn count[T:Bump](own cursor:Iterator[T,never])->i64:
  total=0
  for value in cursor:
    total=total+Bump.bump(value)
  return total
fn values[T](items:[T])->Iterator[T,never]:
  return for value in items:
    yield value
fn main()->i64:
  return count(values([0,1,2]))
""",
        6,
        "",
    ),
    "scalar-snapshot": (
        """fn main()->i64:
  a=0
  cursor=while a<3:
    a=a+1
    yield a
  a=99
  total=0
  for value in cursor:
    total=total+value
  return total+a
""",
        105,
        "",
    ),
    "outlives-array-creator": (
        """fn values()->Iterator[i64,never]:
  data=[3,4]
  return for value in data:
    yield value
fn main()->i64:
  cursor=values()
  total=0
  for value in cursor:
    total=total+value
  return total
""",
        7,
        "",
    ),
    "scalar": (
        """fn squares(start:i64)->Iterator[i64,never]:
  return for value in range(start,start+3):
    yield value*value
fn main()->i64:
  cursor=squares(4)
  total=0
  for value in cursor:
    total=total+value
  return total
""",
        77,
        "",
    ),
    "effects-and-close": (
        """from "std/io" import println
fn emit(text:String)->unit:
  done=println(text) catch fn(error):return ()
  return ()
fn values()->Iterator[i64,never]:
  return for value in range(4):
    emit("step")
    yield value
fn main()->i64:
  cursor=values()
  emit("created")
  total=0
  for value in cursor:
    total=total+value
    if value==1:
      break
  return total
""",
        1,
        "created\nstep\nstep\n",
    ),
    "typed-error-after-cleanup": (
        """error Problem:
  Bad:
    message:String
fn values()->Iterator[i64,Problem]:
  return for value in range(4):
    if value==2:
      fail Problem.Bad {message:"er"+"ror"}
    yield value
fn main()->i64:
  cursor=values()
  total=0
  result=for value in cursor:
    total=total+value
  else:total
  return result catch fn(error):
    return match error:
      Problem.Bad {message}:
        if message=="error":42
        else:-1
""",
        42,
        "",
    ),
    "array-snapshot": (
        """fn main()->i64:
  cursor=for i in range(3):
    values=[i,10]
    yield copy(values)
    values[0]=100
  total=0
  for values in cursor:
    total=total+values[0]
  return total
""",
        3,
        "",
    ),
    "owned-buffer-capture": (
        """from "std/buffer" import ByteBuffer,BufferError,byteBuffer,append,length
fn values(own buffer:ByteBuffer)->Iterator[i64,BufferError]:
  return for i in range(3):
    try append(buffer,[u8(7)])
    yield length(buffer)
fn run()->i64 ! BufferError:
  buffer=try byteBuffer(0)
  cursor=values(buffer)
  total=0
  return try for value in cursor:
    total=total+value
  else:total
fn main()->i64:
  return run() catch fn(error):return -1
""",
        6,
        "",
    ),
    "parallel-across-yield": (
        """fn child(value:i64)->i64:
  return value*10
fn values()->Iterator[i64,TaskError]:
  return for i in range(3):
    result=try parallel:
      pending=spawn child(i)
      yield i
      try await pending
fn main()->i64:
  cursor=values()
  total=0
  result=for value in cursor:
    total=total+value
  else:total
  return result catch fn(error):return -1
""",
        3,
        "",
    ),
    "prefetch-close-cancels-timer": (
        """from "std/time" import sleep,nanoseconds,TimeError
fn child()->i64 ! TimeError:
  try sleep(nanoseconds(u64(3600000000000)))
  return 7
type ChildrenError=TaskError | TimeError
fn values()->Iterator[i64,ChildrenError]:
  return for i in range(1):
    result=try parallel:
      pending=spawn child()
      yield i
      try await pending
fn run()->i64 ! (TaskError | TimeError):
  cursor=values()
  item=try next(cursor)
  try sleep(nanoseconds(u64(1000000)))
  close(cursor)
  return match item:
    Option.Some {value}:value+42
    Option.None:-1
fn main()->i64:
  return run() catch fn(error):return -1
""",
        42,
        "",
    ),
    "retained-function-item": (
        """fn choose()->fn(i64)->i64:
  cursor=for i in range(3):
    yield fn(value:i64)->i64:return value+i
  return for callback in cursor:
    break callback
  else:fn(value:i64)->i64:return value
fn main()->i64:
  callback=choose()
  return callback(42)
""",
        42,
        "",
    ),
    "task-error-as-value": (
        """fn divide(value:i64)->i64:
  return 1/value
fn values(error:TaskError)->Iterator[i64,TaskError]:
  return for i in range(2):
    yield i
    fail error
fn recover(error:TaskError)->i64:
  cursor=values(error)
  result=for value in cursor:
    ()
  else:0
  return result catch fn(error):
    return match error:
      TaskError.Fault {kind,message}:42
      _:-1
fn main()->i64:
  result=parallel:
    try await spawn divide(0)
  return result catch fn(error):return recover(error)
""",
        42,
        "",
    ),
    "mixed-task-error-as-value": (
        """error Problem:
  Bad:
    message:String
type Both=Problem | TaskError
fn divide(value:i64)->i64:
  return 1/value
fn values(error:TaskError)->Iterator[i64,Both]:
  return for i in range(3):
    if i==2:
      fail Problem.Bad {message:"wrong"}
    yield i
    fail error
fn recover(error:TaskError)->i64:
  cursor=values(error)
  result=for value in cursor:
    ()
  else:0
  return result catch fn(error):
    return match error:
      TaskError.Fault {kind,message}:42
      _:-1
fn main()->i64:
  result=parallel:
    try await spawn divide(0)
  return result catch fn(error):return recover(error)
""",
        42,
        "",
    ),
}

source, result, output = cases["generic-materialize-error"]
cases["generic-fresh-materialize-error"] = (
    source.replace(
        "  return try array(cursor)",
        "  return try array(for item in cursor:\n    yield item\n  )",
    ),
    result,
    output,
)

source, result, output = cases["tensor-item-outlives-close"]
cases["tensor-item-coordinating"] = (
    """from "std/io" import println
fn emit()->unit:
  done=println("step") catch fn(error):return ()
  return ()
"""
    + source.replace("    yield tensor(", "    emit()\n    yield tensor("),
    result,
    "step\n",
)

prefetch_source, prefetch_result, prefetch_output = cases[
    "prefetch-close-cancels-timer"
]
cases["adapter-prefetch-close"] = (
    'from "std/iter" import map as mapItems\n'
    + prefetch_source.replace(
        "  cursor=values()",
        "  cursor=mapItems(values(),fn(value:i64)->i64:return value)",
    ),
    prefetch_result,
    prefetch_output,
)


for primitive in ("i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"):
    cases["numeric-items-" + primitive] = (
        f"""fn values()->Iterator[{primitive},never]:
  return for i in range(3):
    yield {primitive}(i+1)
fn main()->i64:
  total=0
  for value in values():
    total=total+i64(value)
  return total
""",
        6,
        "",
    )

for coordinating in (False, True):
    cases["recursive-items-" + ("coordinating" if coordinating else "synchronous")] = (
        (
            """from "std/io" import println
fn emit()->unit:
  done=println("step") catch fn(error):return ()
  return ()
"""
            if coordinating
            else ""
        )
        + """enum List:
  Nil
  Cons:
    head:i64
    tail:List
fn values()->Iterator[List,never]:
  return for value in range(3):
"""
        + ("    emit()\n" if coordinating else "")
        + """    yield List.Cons {head:value,tail:List.Nil}
fn main()->i64:
  cursor=values()
  total=0
  for item in cursor:
    value=match item:
      List.Nil:0
      List.Cons {head,tail}:head
    total=total+value
  return total
""",
        3,
        "step\n" * 3 if coordinating else "",
    )

for bits in (32, 64):
    source, expected, output = cases[f"fusion-f{bits}-rounding"]
    cases[f"fusion-f{bits}-rounding-take"] = (
        'from "std/iter" import take\n'
        + source.replace(
            "  total=0\n  for value in values:",
            "  limited=take(values,3)\n  total=0\n  for value in limited:",
        ),
        expected,
        output,
    )
