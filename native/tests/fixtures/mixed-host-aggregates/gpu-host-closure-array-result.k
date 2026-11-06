fn make(values:[int])->fn(int)->[int]:
  fn(n:int):
    if n==0:values
    else:map(values,fn(value:int):value+n)
fn work(callback:fn(int)->[int],n:int)->[int]:
  callback(n)
fn main()->int ! TaskError:
  values=[7,11]
  callback=make(values)
  result=try parallel:
    try await spawn work(callback,0) on devices.gpu()
  result[0]=19
  fresh=try parallel:
    try await spawn work(callback,3) on devices.gpu()
  values[0]+result[1]+fresh[0]+fresh[1]
