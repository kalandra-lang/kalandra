fn main()->int ! TaskError:
  callbacks=try parallelMap([40],fn(offset):fn(value:int):offset+value)
  callbacks[0](2)
