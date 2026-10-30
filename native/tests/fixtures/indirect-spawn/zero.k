fn double(value:int)->int:
  value*2
fn answer()->int:
  42
fn main()->int ! TaskError:
  f=answer
  try parallel:
    try await spawn f()
