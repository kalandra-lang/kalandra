type Callback:
  run:fn(i64)->i64
  values:[i64]
fn named(n:i64)->i64:
  return n*2
fn make(n:i64)->Callback:
  values=[n,n+1]
  first=fn(value:i64):return value+values[0]
  nested=fn(value:i64):return first(value)+values[1]
  selected=if n<0:named
  else:nested
  return Callback {run:selected,values:values}
fn work(n:i64)->i64:
  return n+1
fn main()->i64 ! TaskError:
  original=make(3)
  other=make(-1)
  for i in range(20):
    temporary=make(i)
  result=try parallel:
    try await spawn work(original.run(1)) on devices.gpu()
  return original.run(result)+other.run(2)+original.values[0]
