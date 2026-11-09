fn main()->i64 ! TaskError:
  offsets=[40]
  callbacks=[fn(value:i64):return offsets[0]+value]
  return try parallel:
    values=try await spawn map(callbacks,fn(callback):return callback(2))
    values[0]
