fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn run(flag:bool)->i64 ! TaskError:
  offset=40
  text="é"+"!"
  f=if flag:fn(value:i64):return value+offset
  else:fn(value:i64):return value+length(text)
  value=if flag:2
  else:40
  return try parallel:
    try await spawn apply(value,f)
fn main()->i64 ! TaskError:
  a=try run(true)
  b=try run(false)
  return a+b-42
