fn apply(callbacks:[fn(int)->int])->int:
  callbacks[0](1)+callbacks[1](1)
fn main()->int ! TaskError:
  values=[20]
  f=fn(value:int):values[0]+value
  try parallel:
    try await spawn apply([f,f])
