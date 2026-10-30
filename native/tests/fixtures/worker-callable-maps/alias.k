fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  f=double
  try parallel:
    values=try await spawn map([21],f)
    values[0]
