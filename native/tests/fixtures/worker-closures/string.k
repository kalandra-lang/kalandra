fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  text="é"+"🙂"
  f=fn(value:int):value+length(text)
  try parallel:
    try await spawn apply(40,f)
