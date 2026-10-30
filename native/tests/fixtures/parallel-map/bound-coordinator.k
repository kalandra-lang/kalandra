fn double(value:int)->int:
  value*2
fn worker(value:int)->int ! TaskError:
  try parallel:
    try await spawn double(value)
fn main()->int ! TaskError:
  callback=worker
  values=try parallelMap([21],callback)
  values[0]
