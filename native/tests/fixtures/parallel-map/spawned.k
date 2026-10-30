fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  try parallel:
    work=spawn parallelMap([21],double)
    values=try await work
    values[0]
