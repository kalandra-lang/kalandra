fn affineRounds(x:f64,offset:f64,rounds:i64)->f64:
  return if rounds == 0:
    x
  else:
    affineRounds(x*f64(0.5)+offset,offset,rounds-1)
fn affine(seed:i64,rounds:i64)->f64:
  return affineRounds(f64(seed)*f64(0.25),f64(seed)*f64(0.0625),rounds)

fn polynomialRounds(x:f64,offset:f64,rounds:i64)->f64:
  return if rounds == 0:
    x
  else:
    polynomialRounds(x*f64(1.015625)+offset,offset,rounds-1)
fn polynomial(seed:i64,rounds:i64)->f64:
  return polynomialRounds(f64(seed)*f64(0.015625),f64(seed)*f64(0.00390625),rounds)

fn gcd(a:i64,b:i64)->i64:
  return if b == 0:
    a
  else:
    gcd(b,a%b)
fn gcdRounds(x:i64,seed:i64,rounds:i64)->f64:
  return if rounds == 0:
    f64(x)
  else:
    gcdRounds(gcd(x*13+7,seed*7+11)+seed,seed,rounds-1)
fn euclid(seed:i64,rounds:i64)->f64:
  return gcdRounds(seed,seed,rounds)
fn main()->f64:
  return affine(17,16)
