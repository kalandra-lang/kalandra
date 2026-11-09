fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  values=[make(40),make(40)]
  return try parallel:
    count=try await spawn length(values)
    count+40
