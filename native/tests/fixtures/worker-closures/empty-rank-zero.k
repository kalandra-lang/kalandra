fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  empty:[i64]=[]
  scalar= tensor([40],[])
  f=fn(value:i64):return value+length(empty)+scalar[]
  return try parallel:
    try await spawn apply(2,f)
