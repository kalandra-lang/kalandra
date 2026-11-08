fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  values=[20,20]
  base=fn(value:i64):return values[0]+values[1]+value
  middle=fn(value:i64):return base(value)
  outer=fn(value:i64):return middle(value)
  return try parallel:
    work=spawn apply(2,outer)
    first=try await work
    second=try await work
    first+second-42
