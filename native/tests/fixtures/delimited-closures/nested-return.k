fn main()->i64:
  callbacks=map([20],fn(offset):
    return fn(value:i64):
      total=value+offset
      return total
  )
  return callbacks[0](22)
