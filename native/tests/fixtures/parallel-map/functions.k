fn main()->i64 ! TaskError:
  numbers=[40]
  values=try parallelMap([fn(value:i64):return numbers[0]+value],fn(callback):return callback(2))
  return values[0]
