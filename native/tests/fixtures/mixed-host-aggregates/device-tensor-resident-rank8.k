fn make(n:i64)->Tensor[i64, ?, ?, ?, ?, ?, ?, ?, ?]:
  cells=generate(n*2,fn(i):return i+3)
  return tensor(cells,[1,n,1,1,1,1,1,2])

fn score(grid:Tensor[i64, ?, ?, ?, ?, ?, ?, ?, ?])->i64:
  dimensions=shape(grid)
  return grid[0,dimensions[1]-1,0,0,0,0,0,dimensions[7]-1]+dimensions[1]*10+dimensions[7]

fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    grid=try await spawn make(3) on gpu
    try await spawn score(grid) on gpu
