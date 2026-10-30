fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  values=[make(40),make(40)]
  try parallel:
    count=try await spawn length(values)
    count+40
