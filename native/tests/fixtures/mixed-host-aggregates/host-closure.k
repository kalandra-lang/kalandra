fn make(offset:int)->fn(int)->int:
  values=[offset,offset+1]
  fn(value:int):value+values[0]+values[1]
fn work(n:int)->int:
  n*2
fn main()->int ! TaskError:
  callback=make(9)
  first=callback(1)
  result=try parallel:
    try await spawn work(first) on devices.gpu()
  callback(result)
