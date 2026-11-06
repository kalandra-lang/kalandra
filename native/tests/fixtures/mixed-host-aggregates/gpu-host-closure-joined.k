type Callback:
  run:fn(int)->int
  values:[int]
fn make(values:[int])->Callback:
  first=fn(value:int):value+values[0]
  Callback {run:first,values:values}
fn work(values:[int],callback:Callback,other:fn(int)->int,n:int)->int:
  callback.run(n)+other(n)+values[0]+callback.values[1]
fn main()->int ! TaskError:
  values=[7,11]
  callback=make(values)
  other=fn(n:int):n+values[1]
  try parallel:
    try await spawn work(values,callback,other,3) on devices.gpu()
