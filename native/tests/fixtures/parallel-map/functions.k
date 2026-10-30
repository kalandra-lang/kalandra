fn main()->int ! TaskError:
  numbers=[40]
  values=try parallelMap([fn(value:int):numbers[0]+value],fn(callback):callback(2))
  values[0]
