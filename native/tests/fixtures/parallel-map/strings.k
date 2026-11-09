fn main()->i64 ! TaskError:
  values=try parallelMap(["hello","world"],fn(value):return value+"!")
  return length(values[0])+length(values[1])+30
