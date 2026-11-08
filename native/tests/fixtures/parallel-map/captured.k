fn main()->i64 ! TaskError:
  numbers=[40]
  values=try parallelMap([2],fn(value):return numbers[0]+value)
  return values[0]
