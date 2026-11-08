fn main()->i64 ! TaskError:
  callbacks=try parallelMap([40],fn(offset):return fn(value:i64):return offset+value)
  return callbacks[0](2)
