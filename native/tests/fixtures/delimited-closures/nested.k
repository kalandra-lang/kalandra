fn main()->int:
  offset:int=10
  values=map([10,12],fn(value):
    inner=map([value],fn(item):
      adjusted=item+offset
      adjusted
    )
    inner[0]
  )
  values[0]+values[1]
