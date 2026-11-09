fn rankZero(n:i64)->Tensor[i64]:
  cells=[n]
  return tensor(cells,[])
fn empty(n:i64)->Tensor[i64, ?, ?]:
  cells=[n]
  return tensor(slice(cells,0,0),[0,n])
fn score(a:Tensor[i64],b:Tensor[i64, ?, ?])->i64:
  return a[]+shape(b)[1]+length(array(b))
fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    first=spawn rankZero(7) on gpu
    second=spawn empty(3) on gpu
    a=try await first
    b=try await second
    try await spawn score(a,b) on gpu
