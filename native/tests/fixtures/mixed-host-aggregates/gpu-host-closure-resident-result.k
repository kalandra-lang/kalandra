fn make(n:int)->fn(int)->Tensor[int, ?, ?]:
  base=tensor([n,n+1],[1,2])
  fn(i:int):base+tensor([i,i],[1,2])
fn work(callback:fn(int)->Tensor[int, ?, ?],grid:Tensor[int, ?, ?])->Tensor[int, ?, ?]:
  callback(7)+grid
fn score(grid:Tensor[int, ?, ?])->int:
  grid[0,0]+grid[0,1]
fn main()->int ! TaskError:
  callback=make(5)
  try parallel:
    gpu=devices.gpu()
    grid=tensor([3,4],[1,2])
    resident=try await transfer(grid,gpu)
    result=try await spawn work(callback,resident) on gpu
    try await spawn score(result) on gpu
