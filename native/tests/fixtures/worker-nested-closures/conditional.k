fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn run(flag:Bool)->int ! TaskError:
  values=[40]
  first=fn(value:int):values[0]+value
  text="hi"
  other=fn(value:int):length(text)+value+38
  selected=if flag:first
  else:other
  outer=fn(value:int):selected(value)
  try parallel:
    try await spawn apply(2,outer)
fn main()->int ! TaskError:
  (try run(true))+(try run(false))-42
