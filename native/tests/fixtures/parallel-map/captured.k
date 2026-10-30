fn main()->int ! TaskError:
  numbers=[40]
  values=try parallelMap([2],fn(value):numbers[0]+value)
  values[0]
