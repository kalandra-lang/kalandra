fn make(n:int)->fn(int)->int:
  values=[n,n+1,n+2,n+3]
  grid=tensor(values,[2,2])
  high=tensor([n,n+1],[1,1,1,1,1,1,1,2])
  cell=tensor([n],[])
  empty=tensor(slice(values,0,0),[0,n])
  fn(i:int):grid[i,1]+cell[]+shape(empty)[1]+length(array(empty))+high[0,0,0,0,0,0,0,1]
fn work(callback:fn(int)->int)->int:
  callback(1)
fn main()->int ! TaskError:
  callback=make(4)
  try parallel:
    try await spawn work(callback) on devices.gpu()
