fn descend(n:int)->int:
  if n==0:
    0
  else:
    descend(n-1)+1
fn apply(n:int,f:fn(int)->int)->int:
  count=descend(100)
  f(n)+count
fn entry(n:int)->int:
  values=[3]
  factor=5
  f=if n<0:
    fn(value:int):value+values[0]
  else:
    fn(value:int):value*factor
  wrapper=fn(value:int):f(value)+2
  apply(n,wrapper)
fn main()->int ! TaskError:
  try parallel:
    first=spawn entry(-8) on devices.gpu()
    second=spawn entry(8) on devices.gpu()
    (try await first)+(try await second)
