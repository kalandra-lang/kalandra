fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  text="é"+"🙂"
  f=fn(value:i64):return value+length(text)
  return try parallel:
    try await spawn apply(40,f)
