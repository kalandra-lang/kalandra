fn main()->int:
  values=map(
    [20,21],
    fn(value):
      adjusted=value+1
      adjusted
  )
  values[0]+values[1]-1
