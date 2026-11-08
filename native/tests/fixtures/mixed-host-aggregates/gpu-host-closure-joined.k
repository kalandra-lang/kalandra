type Callback:
  run:fn(i64)->i64
  values:[i64]
fn make(values:[i64])->Callback:
  first=fn(value:i64):return value+values[0]
  return Callback {run:first,values:values}
fn work(values:[i64],callback:Callback,other:fn(i64)->i64,n:i64)->i64:
  return callback.run(n)+other(n)+values[0]+callback.values[1]
fn main()->i64 ! TaskError:
  values=[7,11]
  callback=make(values)
  other=fn(n:i64):return n+values[1]
  return try parallel:
    try await spawn work(values,callback,other,3) on devices.gpu()
