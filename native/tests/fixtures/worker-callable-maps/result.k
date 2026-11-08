fn main()->i64 ! TaskError:
  callbacks=try parallel:
    try await spawn map([40],fn(offset):return fn(value:i64):return offset+value)
  return callbacks[0](2)
