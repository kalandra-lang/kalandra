fn entry(grid:Tensor[int, ?, 2])->int:
  dimensions=shape(grid)
  changed=reshape(-(grid+1),[2,dimensions[0]])
  copyOfInput=array(grid)
  copyOfInput[0]=99
  grid[0,0]+changed[1,0]+copyOfInput[0]+dimensions[0]
fn main()->int ! TaskError:
  try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    try await spawn entry(resident) on gpu
