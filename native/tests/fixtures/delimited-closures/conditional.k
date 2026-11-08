fn main()->i64:
  values=map([-20,22],fn(value):
    return if value<0:
      adjusted=0-value
      adjusted
    else:
      adjusted=value
      adjusted
  )
  return values[0]+values[1]
