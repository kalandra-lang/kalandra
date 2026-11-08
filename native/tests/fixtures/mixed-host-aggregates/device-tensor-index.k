fn entry(rows:i64)->i64:
  cells=[7,11,17,19]
  grid=tensor(cells,[rows,4/rows])
  return grid[0,0]
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn entry(2) on devices.gpu()
