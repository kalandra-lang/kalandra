fn main()->int ! TaskError:
  values=try parallelMap([true,false],fn(value):not value)
  if values[1]:42
  else:0
