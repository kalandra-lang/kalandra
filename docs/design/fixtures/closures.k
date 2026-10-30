fn makeAdder(offset:int)->fn(int)->int:
  fn(value:int):value+offset
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
export fn main()->int:
  add=makeAdder(10)
  apply(22,add)+add(0)
