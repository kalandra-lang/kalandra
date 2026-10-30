fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  empty:[int]=[]
  scalar= tensor([40],[])
  f=fn(value:int):value+length(empty)+scalar[]
  try parallel:
    try await spawn apply(2,f)
