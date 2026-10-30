fn main()->int:
  offset:int=10
  values=map([10,12],fn(value):
    shifted=value+offset
    shifted
  )
  values[0]+values[1]
