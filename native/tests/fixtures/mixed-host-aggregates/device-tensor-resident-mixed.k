fn entry(before:[int], grid:Tensor[int, ?, ?], after:[int])->int:
  total=before[0]+after[0]
  for i in range(2):
    total=total+grid[i,0]
  total
fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    try await spawn entry([6],resident,[7]) on gpu
