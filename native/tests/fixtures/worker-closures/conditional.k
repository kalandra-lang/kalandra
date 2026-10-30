fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn run(flag:Bool)->int ! TaskError:
  offset=40
  text="é"+"!"
  f=if flag:fn(value:int):value+offset
  else:fn(value:int):value+length(text)
  value=if flag:2
  else:40
  try parallel:
    try await spawn apply(value,f)
fn main()->int ! TaskError:
  a=try run(true)
  b=try run(false)
  a+b-42
