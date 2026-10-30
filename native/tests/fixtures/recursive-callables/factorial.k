error MathError:
  Negative
fn factorial(n:int)->int ! MathError:
  if n<0:fail MathError.Negative
  if n<=1:
    1
  else:
    previous=try factorial(n-1)
    n*previous
fn main()->int:
  callback=factorial
  callback(5) catch fn(error):0
