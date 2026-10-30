fn main()->int:
  callbacks=map([20],fn(offset):
    fn(value:int):
      total=value+offset
      total
  )
  callbacks[0](22)
