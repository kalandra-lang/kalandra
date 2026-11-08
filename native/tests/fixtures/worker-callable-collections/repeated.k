fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  values=[make(20)]
  return try parallel:
    task=spawn concat(values,values)
    first=try await task
    second=try await task
    first[0](1)+second[1](1)
