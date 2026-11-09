fn descend(n:i64)->i64:
  return if n==0:
    0
  else:
    descend(n-1)+1
fn apply(n:i64,f:fn(i64)->i64)->i64:
  count=descend(100)
  return f(n)+count
fn entry(n:i64)->i64:
  values=[3]
  factor=5
  f=if n<0:
    fn(value:i64):return value+values[0]
  else:
    fn(value:i64):return value*factor
  wrapper=fn(value:i64):return f(value)+2
  return apply(n,wrapper)
fn main()->i64 ! TaskError:
  return try parallel:
    first=spawn entry(-8) on devices.gpu()
    second=spawn entry(8) on devices.gpu()
    (try await first)+(try await second)
