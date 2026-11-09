fn gcd(a:i64,b:i64)->i64:
  return if b == 0:
    a
  else:
    gcd(b,a%b)

fn polynomial(x:i64,depth:i64)->f64:
  return if depth == 0:
    value=f64(x)
    (value*0.125+0.25)*value-0.5
  else:
    polynomial(x*2,depth-1)+polynomial(x*2+1,depth-1)

fn branches(x:i64,depth:i64)->f64:
  return if depth == 0:
    if x%3 == 0:
      f64(x)*0.5
    else:
      if x%3 == 1:
        f64(x)*(-0.25)
      else:
        f64(x)+0.125
  else:
    branches(x*2,depth-1)+branches(x*2+1,depth-1)

fn euclid(x:i64,depth:i64)->f64:
  return if depth == 0:
    f64(gcd(x*13+7,x*7+11))
  else:
    euclid(x*2,depth-1)+euclid(x*2+1,depth-1)

fn main()->f64:
  return polynomial(17,4)
