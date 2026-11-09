fn main()->i64 ! TaskError:
  numbers=[40]
  callback=fn(value:i64):return numbers[0]+value
  values=try parallelMap([2],callback)
  return values[0]
