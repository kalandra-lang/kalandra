fn main()->int ! TaskError:
  offsets=[40]
  f=fn(value:int):offsets[0]+value
  try parallel:
    values=try await spawn map([2],fn(value):f(value))
    values[0]
