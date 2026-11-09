fn main()->i64:
  values=map(
    [20,21],
    fn(value):
      adjusted=value+1
      return adjusted
  )
  return values[0]+values[1]-1
