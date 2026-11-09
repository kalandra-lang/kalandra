fn make(values:[i64])->fn(i64)->[i64]:
  return fn(n:i64):
    return if n==0:values
    else:map(values,fn(value:i64):return value+n)
fn work(callback:fn(i64)->[i64],n:i64)->[i64]:
  return callback(n)
fn main()->i64 ! TaskError:
  values=[7,11]
  callback=make(values)
  result=try parallel:
    try await spawn work(callback,0) on devices.gpu()
  result[0]=19
  fresh=try parallel:
    try await spawn work(callback,3) on devices.gpu()
  return values[0]+result[1]+fresh[0]+fresh[1]
