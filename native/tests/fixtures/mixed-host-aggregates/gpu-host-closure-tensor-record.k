type Grid:
  values:Tensor[int,2,2]
fn make(n:int)->fn(int)->int:
  grid=Grid {values:tensor([n,n+1,n+2,n+3],[2,2])}
  fn(i:int):grid.values[i,1]
fn work(callback:fn(int)->int)->int:
  callback(1)
fn main()->int ! TaskError:
  callback=make(4)
  try parallel:
    try await spawn work(callback) on devices.gpu()
