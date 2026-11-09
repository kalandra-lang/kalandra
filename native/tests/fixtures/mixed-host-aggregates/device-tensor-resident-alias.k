fn choose(before:[i64], grid:Tensor[i64, ?, ?], after:[i64])->[i64]:
  return if grid[0,0] == 1:
    after
  else:
    before
fn main()->i64 ! TaskError:
  return try parallel:
    gpu=devices.gpu()
    grid=tensor([1,2,3,4],[2,2])
    resident=try await transfer(grid,gpu)
    before=[6]
    after=[7]
    selected=try await spawn choose(before,resident,after) on gpu
    selected[0]=9
    before[0]*100+after[0]
