fn main()->[int]:
  offset:int=10
  map([-20,0,5],fn(value):
    shifted=value+offset
    if shifted>0:
      shifted
    else:
      0
  )
