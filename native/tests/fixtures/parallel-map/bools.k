fn main()->i64 ! TaskError:
  values=try parallelMap([true,false],fn(value):return not value)
  return if values[1]:42
  else:0
