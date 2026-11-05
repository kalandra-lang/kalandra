fn make(n:int)->Tensor[int, ?, ?, ?, ?, ?, ?, ?, ?]:
  cells=generate(n*2,fn(i):i+3)
  tensor(cells,[1,n,1,1,1,1,1,2])

fn score(grid:Tensor[int, ?, ?, ?, ?, ?, ?, ?, ?])->int:
  dimensions=shape(grid)
  grid[0,dimensions[1]-1,0,0,0,0,0,dimensions[7]-1]+dimensions[1]*10+dimensions[7]

fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    grid=try await spawn make(3) on gpu
    try await spawn score(grid) on gpu
