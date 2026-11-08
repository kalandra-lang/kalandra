fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn run(flag:bool)->i64 ! TaskError:
  values=[40]
  first=fn(value:i64):return values[0]+value
  text="hi"
  other=fn(value:i64):return length(text)+value+38
  selected=if flag:first
  else:other
  outer=fn(value:i64):return selected(value)
  return try parallel:
    try await spawn apply(2,outer)
fn main()->i64 ! TaskError:
  return (try run(true))+(try run(false))-42
