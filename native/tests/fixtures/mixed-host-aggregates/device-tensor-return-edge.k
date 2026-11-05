fn rankZero(n:int)->Tensor[int]:
  cells=[n]
  tensor(cells,[])
fn empty(n:int)->Tensor[int, ?, ?]:
  cells=[n]
  tensor(slice(cells,0,0),[0,n])
fn score(a:Tensor[int],b:Tensor[int, ?, ?])->int:
  a[]+shape(b)[1]+length(array(b))
fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    first=spawn rankZero(7) on gpu
    second=spawn empty(3) on gpu
    a=try await first
    b=try await second
    try await spawn score(a,b) on gpu
