fn entry(n:int)->int:
  raw=[7,11,17,19]
  columns=4/n
  grid=tensor(raw,[n,columns])
  negative=-(grid+grid+1)
  rotated=reshape(negative,[columns,n])
  flat=array(rotated)
  flat[0]=99
  dims=shape(grid)
  grid[0,0]+rotated[0,0]+flat[0]+dims[0]*10+dims[1]
fn run(n:int)->int ! TaskError:
  try parallel:
    try await spawn entry(n) on devices.gpu()
fn main()->int ! TaskError:
  invalid=run(3) catch fn(error):
    match error:
      TaskError.Fault {kind}:
        match kind:
          FaultKind.Shape:1000
          _:0
      _:0
  (try run(1))+(try run(2))+(try run(4))+invalid
