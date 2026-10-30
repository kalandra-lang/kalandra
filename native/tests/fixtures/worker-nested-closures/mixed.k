fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  text="é🙂"
  size=fn(value:String):length(value)
  extra=fn():40
  outer=fn(value:int):size(text)+extra()+value
  try parallel:
    try await spawn apply(0,outer)
