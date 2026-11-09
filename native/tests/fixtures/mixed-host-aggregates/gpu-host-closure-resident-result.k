fn make(n:i64)->fn(i64)->Tensor[i64, ?, ?]:
  base=tensor([n,n+1],[1,2])
  return fn(i:i64):return base+tensor([i,i],[1,2])
fn work(callback:fn(i64)->Tensor[i64, ?, ?],grid:Tensor[i64, ?, ?])->Tensor[i64, ?, ?]:
  return callback(7)+grid
fn score(grid:Tensor[i64, ?, ?])->i64:
  return grid[0,0]+grid[0,1]
fn main()->i64 ! TaskError:
  callback=make(5)
  return try parallel:
    gpu=devices.gpu()
    grid=tensor([3,4],[1,2])
    resident=try await transfer(grid,gpu)
    result=try await spawn work(callback,resident) on gpu
    try await spawn score(result) on gpu
