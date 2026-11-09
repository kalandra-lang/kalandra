fn make(n:i64)->fn(i64)->i64:
  values=[n,n+1,n+2,n+3]
  grid=tensor(values,[2,2])
  high=tensor([n,n+1],[1,1,1,1,1,1,1,2])
  cell=tensor([n],[])
  empty=tensor(slice(values,0,0),[0,n])
  return fn(i:i64):return grid[i,1]+cell[]+shape(empty)[1]+length(array(empty))+high[0,0,0,0,0,0,0,1]
fn work(callback:fn(i64)->i64)->i64:
  return callback(1)
fn main()->i64 ! TaskError:
  callback=make(4)
  return try parallel:
    try await spawn work(callback) on devices.gpu()
