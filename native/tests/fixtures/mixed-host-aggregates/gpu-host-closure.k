fn make(offset:i64)->fn(i64)->i64:
  values=[offset,offset+1]
  return fn(value:i64):return value+values[0]+values[1]
fn work(callback:fn(i64)->i64,n:i64)->i64:
  return callback(n)
fn main()->i64 ! TaskError:
  callback=make(9)
  return try parallel:
    try await spawn work(callback,3) on devices.gpu()
