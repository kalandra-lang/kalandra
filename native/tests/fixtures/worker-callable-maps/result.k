fn main()->int ! TaskError:
  callbacks=try parallel:
    try await spawn map([40],fn(offset):fn(value:int):offset+value)
  callbacks[0](2)
