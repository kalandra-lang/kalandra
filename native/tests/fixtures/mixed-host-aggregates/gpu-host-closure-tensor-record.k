type Grid:
  values:Tensor[i64,2,2]
fn make(n:i64)->fn(i64)->i64:
  grid=Grid {values:tensor([n,n+1,n+2,n+3],[2,2])}
  return fn(i:i64):return grid.values[i,1]
fn work(callback:fn(i64)->i64)->i64:
  return callback(1)
fn main()->i64 ! TaskError:
  callback=make(4)
  return try parallel:
    try await spawn work(callback) on devices.gpu()
