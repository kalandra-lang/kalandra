type Callback:
  run:fn(int)->int
  values:[int]
fn named(n:int)->int:
  n*2
fn make(n:int)->Callback:
  values=[n,n+1]
  first=fn(value:int):value+values[0]
  nested=fn(value:int):first(value)+values[1]
  selected=if n<0:named
  else:nested
  Callback {run:selected,values:values}
fn work(n:int)->int:
  n+1
fn main()->int ! TaskError:
  original=make(3)
  other=make(-1)
  for i in range(20):
    temporary=make(i)
  result=try parallel:
    try await spawn work(original.run(1)) on devices.gpu()
  original.run(result)+other.run(2)+original.values[0]
