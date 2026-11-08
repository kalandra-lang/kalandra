error MathError:
  Negative
fn factorial(n:i64)->i64 ! MathError:
  if n<0:fail MathError.Negative
  return if n<=1:
    1
  else:
    previous=try factorial(n-1)
    n*previous
fn main()->i64:
  callback=factorial
  return callback(5) catch fn(error):return 0
