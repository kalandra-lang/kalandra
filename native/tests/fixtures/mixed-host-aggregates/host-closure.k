fn make(offset:i64)->fn(i64)->i64:
  values=[offset,offset+1]
  return fn(value:i64):return value+values[0]+values[1]
fn work(n:i64)->i64:
  return n*2
fn main()->i64 ! TaskError:
  callback=make(9)
  first=callback(1)
  result=try parallel:
    try await spawn work(first) on devices.gpu()
  return callback(result)
