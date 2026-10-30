fn make(offset:int)->fn(int)->int:
  numbers=[offset]
  fn(value:int):numbers[0]+value
fn main()->int ! TaskError:
  left=[make(20)]
  right=[make(18)]
  values=try parallel:
    try await spawn concat(left,right)
  values[0](2)+values[1](2)
