type Batch:
  values:[i64]
  score:i64
fn make(values:[i64])->fn(i64)->Batch:
  return fn(n:i64):return Batch {values:values,score:n+values[1]}
fn work(callback:fn(i64)->Batch,n:i64)->Batch:
  return callback(n)
fn main()->i64 ! TaskError:
  values=[7,11]
  callback=make(values)
  result=try parallel:
    try await spawn work(callback,3) on devices.gpu()
  result.values[0]=19
  return values[0]+result.values[1]+result.score
