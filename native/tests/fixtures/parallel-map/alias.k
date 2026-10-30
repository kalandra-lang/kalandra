fn main()->int ! TaskError:
  numbers=[40]
  callback=fn(value:int):numbers[0]+value
  values=try parallelMap([2],callback)
  values[0]
