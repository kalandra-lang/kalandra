fn main()->int ! TaskError:
  values=try parallelMap(["hello","world"],fn(value):value+"!")
  length(values[0])+length(values[1])+30
