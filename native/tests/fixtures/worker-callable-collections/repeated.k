fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  values=[make(20)]
  try parallel:
    task=spawn concat(values,values)
    first=try await task
    second=try await task
    first[0](1)+second[1](1)
