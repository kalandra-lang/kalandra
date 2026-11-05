fn entry(rows:int)->int:
  cells=[7,11,17,19]
  grid=tensor(cells,[rows,4/rows])
  grid[0,0]
fn main()->int ! TaskError:
  try parallel:
    try await spawn entry(2) on devices.gpu()
