fn main()->i64:
  offset:i64=10
  values=map([10,12],fn(value):
    shifted=value+offset
    return shifted
  )
  return values[0]+values[1]
