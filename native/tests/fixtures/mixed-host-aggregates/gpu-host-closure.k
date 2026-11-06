fn make(offset:int)->fn(int)->int:
  values=[offset,offset+1]
  fn(value:int):value+values[0]+values[1]
fn work(callback:fn(int)->int,n:int)->int:
  callback(n)
fn main()->int ! TaskError:
  callback=make(9)
  try parallel:
    try await spawn work(callback,3) on devices.gpu()
