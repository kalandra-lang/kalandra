fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  functions=[double]
  try parallel:
    try await spawn functions[0](21)
