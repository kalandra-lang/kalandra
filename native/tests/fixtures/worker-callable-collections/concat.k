fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  left=[make(20)]
  right=[make(18)]
  values=try parallel:
    try await spawn concat(left,right)
  return values[0](2)+values[1](2)
