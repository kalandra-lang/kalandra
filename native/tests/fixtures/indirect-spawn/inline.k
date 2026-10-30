fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  try parallel:
    try await spawn (fn(value:int):value*2)(21)
