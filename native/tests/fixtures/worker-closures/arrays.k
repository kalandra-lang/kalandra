fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  texts=["é","🙂"]
  flags=[true,false]
  f=fn(value:int):
    if flags[0]:value+length(texts[0])+length(texts[1])
    else:0
  try parallel:
    try await spawn apply(40,f)
