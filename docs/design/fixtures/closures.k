fn makeAdder(offset:i64)->fn(i64)->i64:
  return fn(value:i64):return value+offset
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
export fn main()->i64:
  add=makeAdder(10)
  return apply(22,add)+add(0)
