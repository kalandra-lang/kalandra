fn main()->[i64]:
  offset:i64=10
  return map([-20,0,5],fn(value):
    shifted=value+offset
    return if shifted>0:
      shifted
    else:
      0
  )
