fn even(n:i64)->i64:
  return if n==0:
    1
  else:
    odd(n-1)+1
fn odd(n:i64)->i64:
  return if n==0:
    0
  else:
    even(n-1)+2
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn even(1000) on devices.gpu()
