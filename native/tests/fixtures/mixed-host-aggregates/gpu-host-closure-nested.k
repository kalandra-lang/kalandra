fn named(n:i64)->i64:
  return n*2
fn make(n:i64)->fn(i64)->i64:
  values=[n,n+1]
  first=fn(value:i64):return value+values[0]
  nested=fn(value:i64):return first(value)+values[1]
  return if n<0:named
  else:nested
fn work(callback:fn(i64)->i64,n:i64)->i64:
  return callback(n)
fn main()->i64 ! TaskError:
  original=make(3)
  other=make(-1)
  for i in range(20):
    temporary=make(i)
  return try parallel:
    first=spawn work(original,5) on devices.gpu()
    second=spawn work(other,7) on devices.gpu()
    (try await first)+(try await second)
