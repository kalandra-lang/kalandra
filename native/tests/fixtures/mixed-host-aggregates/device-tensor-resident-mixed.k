fn entry(before:[i64], grid:Tensor[i64, ?, ?], after:[i64])->i64:
  total=before[0]+after[0]
  for i in range(2):
    total=total+grid[i,0]
  return total
fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    try await spawn entry([6],resident,[7]) on gpu
