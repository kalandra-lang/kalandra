fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  values=[40]
  f=fn(value:int):values[0]+value
  try parallel:
    try await spawn f(2)
