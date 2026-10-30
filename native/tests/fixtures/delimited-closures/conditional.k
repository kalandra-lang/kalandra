fn main()->int:
  values=map([-20,22],fn(value):
    if value<0:
      adjusted=0-value
      adjusted
    else:
      adjusted=value
      adjusted
  )
  values[0]+values[1]
