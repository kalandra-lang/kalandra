fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  numbers=[20]
  base=fn(value:i64):return numbers[0]+value
  left=base
  right=base
  outer=fn(value:i64):return left(value)+right(value)
  return try parallel:
    try await spawn apply(1,outer)
