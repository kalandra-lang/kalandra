fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  f=double
  try parallel:
    try await spawn f(21)
