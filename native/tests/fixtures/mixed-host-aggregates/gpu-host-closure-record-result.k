type Batch:
  values:[int]
  score:int
fn make(values:[int])->fn(int)->Batch:
  fn(n:int):Batch {values:values,score:n+values[1]}
fn work(callback:fn(int)->Batch,n:int)->Batch:
  callback(n)
fn main()->int ! TaskError:
  values=[7,11]
  callback=make(values)
  result=try parallel:
    try await spawn work(callback,3) on devices.gpu()
  result.values[0]=19
  values[0]+result.values[1]+result.score
