fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  text="é🙂"
  size=fn(value:String):return length(value)
  extra=fn():return 40
  outer=fn(value:i64):return size(text)+extra()+value
  return try parallel:
    try await spawn apply(0,outer)
