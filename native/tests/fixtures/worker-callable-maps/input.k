fn main()->int ! TaskError:
  offsets=[40]
  callbacks=[fn(value:int):offsets[0]+value]
  try parallel:
    values=try await spawn map(callbacks,fn(callback):callback(2))
    values[0]
