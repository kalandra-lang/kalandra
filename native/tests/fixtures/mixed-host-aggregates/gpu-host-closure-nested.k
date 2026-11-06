fn named(n:int)->int:
  n*2
fn make(n:int)->fn(int)->int:
  values=[n,n+1]
  first=fn(value:int):value+values[0]
  nested=fn(value:int):first(value)+values[1]
  if n<0:named
  else:nested
fn work(callback:fn(int)->int,n:int)->int:
  callback(n)
fn main()->int ! TaskError:
  original=make(3)
  other=make(-1)
  for i in range(20):
    temporary=make(i)
  try parallel:
    first=spawn work(original,5) on devices.gpu()
    second=spawn work(other,7) on devices.gpu()
    (try await first)+(try await second)
