fn double(value:i64)->i64:
  return value*2
fn choose(flag:bool)->fn(i64)->i64:
  return if flag:double
  else:fn(value:i64):return value+21
fn main()->i64 ! TaskError:
  f=choose(false)
  return try parallel:
    try await spawn f(21)
