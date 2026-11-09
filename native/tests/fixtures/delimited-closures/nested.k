fn main()->i64:
  offset:i64=10
  values=map([10,12],fn(value):
    inner=map([value],fn(item):
      adjusted=item+offset
      return adjusted
    )
    return inner[0]
  )
  return values[0]+values[1]
